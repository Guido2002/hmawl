#include "original_startup.h"
#include "native_time_base.h"
#include "native_exi_bus.h"
#include "native_si_bus.h"
#include "native_sram.h"
#include "original_startup_manifest.h"
#include "sha1.h"
#include "disc_import.h"
#include "generated_original_startup.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>

DolImage LoadOriginalUsDol(const std::filesystem::path& path) {
    if (std::filesystem::file_size(path) > 64u * 1024u * 1024u) {
        throw std::runtime_error("Original startup DOL exceeds the 64 MiB input limit.");
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("Could not open original USA DOL.");
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    if (Sha1Hex(bytes) != kOriginalUsDolSha1) {
        throw std::runtime_error("Original startup requires the verified USA revision 0 DOL (SHA-1 mismatch).");
    }
    return DolImage::Parse(bytes);
}

void InstallOriginalStartupDisc(PpcState& state, RvzImage& disc) {
    if (state.pc != 0x80003154u || state.memoryMap != PpcMemoryMap::GameCubeRam ||
        state.callGuest || state.callException || state.discInterface) {
        throw std::runtime_error("Native disc boot metadata requires a fresh unbound USA startup state.");
    }
    const auto& metadata = disc.metadata();
    if (metadata.discId != "GYWEE9" || metadata.discNumber != 0 || metadata.discVersion != 0) {
        throw std::runtime_error("Native startup requires GYWEE9 disc 0, revision 0.");
    }
    const auto files = ReadGameCubeExecutables(disc);
    VerifyUsExecutables(files);
    const auto& fst = files.at(1).bytes;
    const auto id = disc.Read(0, 0x20);
    const auto bi2 = disc.Read(0x440, 0x2000);
    // __start can move ArenaHi to its BI2 argument list; keep the FST above all BI2 bytes.
    constexpr uint32_t bi2Address = 0x81700000u, fstAddress = bi2Address + 0x2000u;
    if (fst.size() > 0x81800000u - fstAddress) {
        throw std::runtime_error("Original disc FST exceeds the native loader's reserved high RAM.");
    }
    const auto fstOffset = ppc_memory_offset(state, fstAddress, fst.size());
    const auto bi2Offset = ppc_memory_offset(state, bi2Address, bi2.size());
    const auto idOffset = ppc_memory_offset(state, 0x80000000u, id.size());
    std::copy(fst.begin(), fst.end(), state.memory.begin() + fstOffset);
    std::copy(bi2.begin(), bi2.end(), state.memory.begin() + bi2Offset);
    std::copy(id.begin(), id.end(), state.memory.begin() + idOffset);
    ppc_write_u32(state, 0x80000020u, 0x0d15ea5eu);
    ppc_write_u32(state, 0x80000030u, 0x8035efa0u);
    ppc_write_u32(state, 0x80000034u, bi2Address);
    ppc_write_u32(state, 0x80000038u, fstAddress);
    ppc_write_u32(state, 0x8000003cu, static_cast<uint32_t>(fst.size()));
    ppc_write_u32(state, 0x800000f4u, bi2Address);
    state.discInterface.emplace();
    state.decrementer.emplace();
    state.transferDisc = [&disc](const std::array<uint32_t, 3>& command, uint32_t length) {
        if (command[0] == 0x12000000u && command[1] == 0 && command[2] == 32 && length == 32) {
            // Explicit native virtual-drive identity: revision 1, no console firmware/date.
            std::vector<uint8_t> inquiry(32);
            inquiry[3] = 1;
            return inquiry;
        }
        if (command[0] == 0xa8000040u && command[1] == 0 && command[2] == 32 && length == 32) {
            return disc.Read(0, 32);
        }
        if (command[0] == 0xa8000000u && command[2] == length) {
            return disc.Read(static_cast<uint64_t>(command[1]) * 4u, length);
        }
        throw std::runtime_error("Unsupported native RVZ drive DMA command.");
    };
}

PpcState CreateOriginalStartupState(const DolImage& image) {
    if (image.entryPoint() != 0x80003154u) {
        throw std::runtime_error("Original USA startup entry point mismatch.");
    }
    auto state = image.CreateGameCubeState();
    // Loader handoff, not reset-vector emulation: translated RAM has IR/DR set.
    state.msr = 0x30u;
    state.externalInterruptsEnabled = false;
    state.piInterruptMask = 0;
    state.piResetCode = 0;
    state.miInterruptMask = 0;
    state.memoryProtection.emplace();
    state.ramBats.emplace();
    state.externalInterrupt.emplace();
    state.dspControl = 0;
    state.dspHardware.emplace();
    state.dspHardware->aram.resize(16u * 1024u * 1024u);
    state.dspHardware->aramControllerReady = true;
    state.dspHardware->disconnectedAramExpansion = true;
    state.executeDspInitialization = ExecuteOriginalDspInitialization;
    state.dspHardware->task.emplace();
    state.executeDspTask = OriginalDspAudioTask;
    state.validateDspTask = ValidateOriginalDspAudioTask;
    state.aiControl = 0;
    state.audio.emplace();
    state.diConfiguration = 1;
    state.viDtvStatus = 0;
    state.serialInterface.emplace();
    state.graphics.emplace();
    state.graphics->efb.emplace();
    state.video.emplace();
    state.transferSi = TransferDisconnectedSi;
    ppc_write_u32(state, 0x800000f8u, 162000000u);
    ppc_write_u32(state, 0x800000fcu, 486000000u);
    ppc_write_u32(state, 0x80000028u, kGameCubeRamSize);
    ppc_write_u32(state, 0x800000f0u, kGameCubeRamSize);
    state.exiStatus = {0u, 0u, 0u};
    state.exiTransferControl = {0u, 0u, 0u};
    state.exiInitializationEnabled = true;
    state.exiImmediateEnabled = true;
    state.transferExiImmediate = TransferEmptyExternalExi;
    BindNativeSram(state, NewNativeSram());
    state.writeConsole = [](const std::string& text) {
        std::cout << text;
        if (!std::cout) throw std::runtime_error("Native console output failed.");
    };
    state.readTimeBase = MakeNativeTimeBase();
    return state;
}

void RegisterOriginalStartup(NativeDispatcher& dispatcher, const PpcState& state) {
    dispatcher.Register(0, state, OriginalStartupTranslations());
}

OriginalStartupResult RunOriginalStartup(NativeDispatcher& dispatcher, PpcState& state,
                                        size_t blockBudget, size_t maxCallDepth,
                                        const std::function<void(uint32_t)>& onBlock) {
    if (state.pc != 0x80003154u || state.callGuest || state.callException ||
        blockBudget == 0 || maxCallDepth == 0) {
        throw std::runtime_error("Original startup requires a fresh entry state and nonzero execution limits.");
    }
    OriginalStartupResult result{};
    const auto finish = [&](OriginalStartupStop stop, const std::string& reason) {
        result.stop = stop;
        result.pc = state.pc;
        result.lr = state.lr;
        result.stack = state.gpr[1];
        result.argument3 = state.gpr[3];
        result.argument4 = state.gpr[4];
        result.argument5 = state.gpr[5];
        result.reason = reason;
        return result;
    };
    try {
        dispatcher.Execute(state, state.pc, blockBudget, maxCallDepth, [&](uint32_t pc) {
            if (pc == 0x80003268u) result.osInitReturned = true;
            if (pc == 0x801b7cc8u) ++result.constructorsReturned;
            if (pc == 0x800032a0u) result.userInitReturned = true;
            if (pc == 0x8000aa54u) result.gameMainEntered = true;
            if (pc == 0x8000aa6cu) result.gameInitializationReturned = true;
            if (pc == 0x8000ad5cu) result.memoryInitialized = true;
            if (pc == 0x8017a3e0u) result.screenInitEntered = true;
            if (pc == 0x801cb378u) result.gxInitEntered = true;
            if (pc == 0x8017a41cu) result.gxInitReturned = true;
            if (pc == 0x801be3bcu) result.viInitEntered = true;
            if (pc == 0x8017a42cu) result.viInitReturned = true;
            if (pc == 0x8017a4c0u) result.viConfigured = true;
            if (pc == 0x8017a674u) result.viRetraceReturned = true;
            if (pc == 0x8000addcu) result.screenInitReturned = true;
            if (pc == 0x80129fa4u) result.arInitReturned = true;
            if (pc == 0x80129fa8u) result.arqInitReturned = true;
            if (pc == 0x80129fb0u) result.aiInitReturned = true;
            if (pc == 0x801f2da0u) result.dspInitReturned = true;
            if (pc == 0x801f2dacu) result.dspTaskAdded = true;
            if (pc == 0x801f0fc4u) result.audioTaskInitReturned = true;
            if (pc == 0x801f2cacu) result.pcmDmaStartReturned = true;
            if (onBlock) onBlock(pc);
        });
    } catch (const std::exception& error) {
        const std::string reason = error.what();
        const bool budget = reason.find("budget exceeded") != std::string::npos ||
                            reason.find("call-depth limit exceeded") != std::string::npos;
        return finish(budget ? OriginalStartupStop::Budget : OriginalStartupStop::Dependency, reason);
    }
    return finish(OriginalStartupStop::UnexpectedReturn,
                  "Original entry returned unexpectedly; no game main loop is available.");
}

std::string DescribeOriginalStartupStop(const OriginalStartupResult& result) {
    std::ostringstream text;
    text << "Original USA startup stopped: PC=0x" << std::hex << std::uppercase
         << std::setfill('0') << std::setw(8) << result.pc
         << " LR=0x" << std::setw(8) << result.lr
         << " SP=0x" << std::setw(8) << result.stack << '\n'
         << "R3=0x" << std::setw(8) << result.argument3
         << " R4=0x" << std::setw(8) << result.argument4
         << " R5=0x" << std::setw(8) << result.argument5 << '\n'
         << result.reason << '\n'
         << (result.osInitReturned ? "Original OSInit returned to __start. " : "OSInit is incomplete. ");
    if (result.constructorsReturned != 0) {
        text << std::dec << result.constructorsReturned << " original C++ constructors returned. ";
    }
    if (result.userInitReturned) text << "Original __init_user returned. ";
    if (result.gameMainEntered) text << "Original game main entered. ";
    if (result.memoryInitialized) text << "Original Memory::Initialize returned. ";
    if (result.screenInitEntered) text << "Original Screen initialization entered. ";
    if (result.gxInitReturned) text << "Original GXInit returned to Screen. ";
    else if (result.gxInitEntered) text << "Original GXInit entered (not completed). ";
    if (result.viInitReturned) text << "Original VIInit returned to Screen. ";
    else if (result.viInitEntered) text << "Original VIInit entered (not completed). ";
    if (result.viConfigured) text << "Original VIConfigure returned. ";
    if (result.viRetraceReturned) text << "Original VIWaitForRetrace returned. ";
    if (result.screenInitReturned) text << "Original Screen constructor/initialization returned. ";
    if (result.arInitReturned) text << "Original ARInit returned. ";
    if (result.arqInitReturned) text << "Original ARQInit returned. ";
    if (result.aiInitReturned) text << "Original AIInit returned. ";
    if (result.dspInitReturned) text << "Original DSPInit returned using the explicit native loader contract. ";
    if (result.dspTaskAdded) text << "Original DSPAddTask returned after IRAM upload. ";
    if (result.audioTaskInitReturned) text << "Original DSP audio-task initialization returned. ";
    if (result.pcmDmaStartReturned) text << "Original AIStartDMA returned with a bound PCM backend. ";
    if (result.gameInitializationReturned) text << "Original initial game-system initialization returned to main. ";
    text << "This is NOT a game boot; no game main loop or renderer ran.";
    return text.str();
}
