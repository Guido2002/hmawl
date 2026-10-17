#include "original_startup.h"
#include "native_time_base.h"
#include "native_exi_bus.h"
#include "native_si_bus.h"
#include "native_sram.h"
#include "original_startup_manifest.h"
#include "sha1.h"
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

PpcState CreateOriginalStartupState(const DolImage& image) {
    if (image.entryPoint() != 0x80003154u) {
        throw std::runtime_error("Original USA startup entry point mismatch.");
    }
    auto state = image.CreateGameCubeState();
    // Loader handoff, not reset-vector emulation: translated RAM has IR/DR set.
    state.msr = 0x30u;
    state.externalInterruptsEnabled = false;
    state.piInterruptMask = 0;
    state.miInterruptMask = 0;
    state.dspControl = 0;
    state.aiControl = 0;
    state.diConfiguration = 1;
    state.viDtvStatus = 0;
    state.serialInterface.emplace();
    state.transferSi = TransferDisconnectedSi;
    ppc_write_u32(state, 0x800000f8u, 162000000u);
    ppc_write_u32(state, 0x800000fcu, 486000000u);
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
                                        size_t blockBudget, size_t maxCallDepth) {
    if (state.pc != 0x80003154u || state.callGuest || blockBudget == 0 || maxCallDepth == 0) {
        throw std::runtime_error("Original startup requires a fresh entry state and nonzero execution limits.");
    }
    try {
        dispatcher.Execute(state, state.pc, blockBudget, maxCallDepth);
    } catch (const std::exception& error) {
        const std::string reason = error.what();
        const bool budget = reason.find("budget exceeded") != std::string::npos ||
                            reason.find("call-depth limit exceeded") != std::string::npos;
        return {budget ? OriginalStartupStop::Budget : OriginalStartupStop::Dependency,
                state.pc, state.lr, state.gpr[1], state.gpr[3], state.gpr[4],
                state.gpr[5], reason};
    }
    return {OriginalStartupStop::UnexpectedReturn, state.pc, state.lr, state.gpr[1],
            state.gpr[3], state.gpr[4], state.gpr[5],
            "Original entry returned unexpectedly; no game main loop is available."};
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
         << "OSInit is incomplete. This is NOT a game boot; no game main loop or renderer ran.";
    return text.str();
}
