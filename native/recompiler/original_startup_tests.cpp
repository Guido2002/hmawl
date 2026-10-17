#include "original_startup.h"
#include "native_console.h"
#include "native_exi_bus.h"
#include "native_si_bus.h"
#include "native_sram.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void RequireFailure(Function function, const char* message) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    Require(failed, message);
}

class DolFixtures {
public:
    explicit DolFixtures(const std::filesystem::path& dol) {
        const auto parent = std::filesystem::absolute(dol).parent_path();
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned attempt = 0; attempt < 100; ++attempt) {
            auto candidate = parent / ("original-startup-fixture-" +
                                      std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) {
                directory_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Could not create a unique build-local DOL fixture directory.");
    }

    ~DolFixtures() {
        std::error_code error;
        std::filesystem::remove_all(directory_, error);
    }

    DolFixtures(const DolFixtures&) = delete;
    DolFixtures& operator=(const DolFixtures&) = delete;

    std::filesystem::path Write(const char* name, const std::vector<uint8_t>& bytes) {
        const auto path = directory_ / name;
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        Require(static_cast<bool>(output), "Could not finish writing build-local DOL fixture.");
        return path;
    }

    void Cleanup() {
        std::filesystem::remove_all(directory_);
        Require(!std::filesystem::exists(directory_), "DOL fixture cleanup failed.");
    }

private:
    std::filesystem::path directory_;
};

} // namespace

int main(int argc, char** argv) {
    try {
        Require(argc == 2 || (argc == 4 && std::string(argv[2]) == "--rvz"),
                "Expected verified USA DOL and optional --rvz USA-disc.rvz.");
        RequireFailure([&] { LoadOriginalUsDol(argv[0]); },
                       "Production DOL loader accepted a nonmatching executable.");
        const auto image = LoadOriginalUsDol(argv[1]);
        DolFixtures fixtures(argv[1]);
        std::ifstream source(argv[1], std::ios::binary);
        Require(static_cast<bool>(source), "Could not read fixture source DOL.");
        std::vector<uint8_t> original((std::istreambuf_iterator<char>(source)), {});
        Require(original.size() > 256, "Fixture source DOL is unexpectedly short.");
        auto truncated = original;
        truncated.resize(128);
        const auto truncatedPath = fixtures.Write("truncated.dol", truncated);
        RequireFailure([&] { LoadOriginalUsDol(truncatedPath); },
                       "Production DOL loader accepted a truncated on-disk DOL.");
        auto mutated = original;
        mutated.back() ^= 1u;
        const auto mutatedPath = fixtures.Write("mutated.dol", mutated);
        RequireFailure([&] { LoadOriginalUsDol(mutatedPath); },
                       "Production DOL loader accepted a mutated on-disk DOL.");
        auto unboundState = CreateOriginalStartupState(image);
        NativeDispatcher unbound;
        const auto noBinding = RunOriginalStartup(unbound, unboundState);
        Require(noBinding.stop == OriginalStartupStop::Dependency &&
                    noBinding.pc == image.entryPoint() && !unboundState.callGuest &&
                    noBinding.reason.find("No native translation registered") != std::string::npos,
                "Unbound production dispatcher did not report its exact missing entry.");
        auto state = CreateOriginalStartupState(image);
        uint64_t deterministicTicks = 1000000;
        state.readTimeBase = [&] { return deterministicTicks += 10000; };
        Require(state.pc == image.entryPoint() && ppc_read_msr(state) == 0x30u &&
                    !state.callGuest && state.readTimeBase && state.piInterruptMask == 0 &&
                    state.miInterruptMask == 0 && state.dspControl == 0 &&
                    state.aiControl == 0 && state.exiStatus[0] == 0 &&
                    state.exiTransferControl[0] == 0 && state.exiInitializationEnabled &&
                    state.serialInterface.has_value() && state.viDtvStatus == 0 &&
                    ppc_read_u32(state, 0x800000f8u) == 162000000u &&
                    ppc_read_u32(state, 0x800000fcu) == 486000000u,
                "Loader profile is not explicit or installs guest service replacements.");
        NativeDispatcher dispatcher;
        RegisterOriginalStartup(dispatcher, state);
        Require(dispatcher.Contains(image.entryPoint()) && dispatcher.Contains(0x80003264u) &&
                    dispatcher.Contains(0x801b0de0u) && dispatcher.Contains(0x801b0f88u),
                "Production bindings omit the original entry-to-OSInit chain.");
        auto missingPeripheral = CreateOriginalStartupState(image);
        missingPeripheral.transferExiImmediate = {};
        const auto missingTransfer = RunOriginalStartup(dispatcher, missingPeripheral);
        Require(missingTransfer.pc == 0x801f6518u &&
                    missingTransfer.reason.find("requires a native peripheral backend") != std::string::npos &&
                    missingPeripheral.exiStatus[0] == 0x2200u &&
                    missingPeripheral.exiTransferControl[0] == 0,
                "Missing EXI backend fabricated response/completion or stopped at the wrong instruction.");
        unsigned transfers = 0;
        unsigned sramCommands = 0;
        const auto peripheralBackend = state.transferExiImmediate;
        state.transferExiImmediate = [&](const ExiImmediateRequest& request) {
            if (request.channel == 0 && request.device == 1) {
                Require(request.direction == 1 && request.length == 4 && request.data == 0x20000100u &&
                            sramCommands++ == 0,
                        "Original SRAM command sequence differs.");
                return peripheralBackend(request);
            }
            Require(request.channel == 0 && request.device == 2 &&
                        request.direction == (transfers == 0 ? 1u : 0u) &&
                        request.length == (transfers == 0 ? 2u : 4u) &&
                        request.data == 0 && transfers < 2,
                    "Original EXI ID command/read sequence differs.");
            ++transfers;
            return peripheralBackend(request);
        };
        unsigned sramDmas = 0;
        const auto dmaBackend = state.transferExiDma;
        state.transferExiDma = [&](const ExiDmaRequest& request) {
            Require(request.channel == 0 && request.device == 1 && request.direction == 0 &&
                        request.length == 64 && request.output.empty() && sramDmas++ == 0,
                    "Original SRAM DMA request differs.");
            return dmaBackend(request);
        };
        std::string console;
        unsigned siTransfers = 0;
        state.transferSi = [&](const SiTransferRequest& request) {
            Require(request.channel == siTransfers && request.output == std::vector<uint8_t>{0} &&
                        request.inputLength == 3 && siTransfers++ < 4,
                    "Original queued SI controller type query differs.");
            return TransferDisconnectedSi(request);
        };
        state.writeConsole = [&](const std::string& text) { console += text; };
        std::fill(state.memory.begin() + 0x35efa0u, state.memory.begin() + 0x1700000u, 0x5a);
        ppc_write_u8(state, 0x81700000u, 0x6b);
        state.dspHardware->dataRam.fill(0xfeedu);
        for (unsigned index = 0; index < 128; ++index) {
            ppc_write_u8(state, 0x81000000u + index, static_cast<uint8_t>(index ^ 0xa5u));
        }
        const auto expectedVersion = ReadGuestConsoleString(state, 0x802e6150u) + '\n' +
            ReadGuestConsoleString(state, ppc_read_u32(state, 0x80348cd8u)) + '\n';
        const auto result = RunOriginalStartup(dispatcher, state);
        std::cout << DescribeOriginalStartupStop(result) << '\n';
        std::cout << "SI transfers=" << siTransfers << " interrupts=" << state.externalInterrupt->delivered
                  << " restored=" << state.externalInterrupt->returned << " SRR0=0x" << std::hex
                  << state.srr0 << " SRR1=0x" << state.srr1 << std::dec << '\n';
        Require(result.stop == OriginalStartupStop::Dependency && result.pc == 0x801b3e00u &&
                    result.lr == 0x801b3eecu && transfers == 2 && siTransfers == 4 &&
                    sramCommands == 1 && sramDmas == 1 &&
                    console.find(expectedVersion) == 0 &&
                    console.find("Memory 24 MB\n") != std::string::npos &&
                    console.find("Arena : 0x8035efa0 - 0x81700000\n") != std::string::npos &&
                    state.exiStatus[0] == 0x2008u &&
                    state.exiTransferControl[0] == 2u &&
                    ppc_read_u32(state, 0x80340c6cu) == 0 &&
                    ppc_read_u32(state, 0x800030c0u) == 0 &&
                    ppc_read_u32(state, 0x800030c4u) == 0 &&
                    result.reason.find("Unsupported PI interrupt mask") != std::string::npos &&
                    ppc_read_u32(state, 0x80349ad0u) == 0,
                "Production startup stopped at an unexpected boundary.");
        Require(state.externalInterrupt && state.externalInterrupt->delivered == 4 &&
                    state.externalInterrupt->returned == 4 && !state.externalInterrupt->active &&
                    state.srr0 == state.externalInterrupt->resume && state.srr1 == 0x8032u &&
                    !state.systemCallActive && !state.callException && state.hid0 == 0xc200u,
                "Original SI exceptions did not return through the original context restore.");
        const PpcRamBatState expectedBats;
        Require(state.ramBats->values == expectedBats.values &&
                    state.ramBats->writes == 12 && state.ramBats->transitions == 2 &&
                    !state.ramBats->realModeActive && state.memoryProtection->permissions == 0xffu &&
                    state.memoryProtection->acknowledgements == 1 &&
                    ppc_read_u32(state, 0x80000028u) == kGameCubeRamSize &&
                    ppc_read_u32(state, 0x800000f0u) == kGameCubeRamSize &&
                    state.miInterruptMask == 0x10,
                "Original memory-protection initialization did not configure real RAM/MI state.");
        for (unsigned index = 0; index < 5; ++index) {
            Require(ppc_read_u32(state, 0x80003040u + index * 4) == 0x801b4ee8u,
                    "Original memory interrupt handler was not registered in every MI slot.");
        }
        for (uint32_t address = 0x8035efa0u; address < 0x81700000u; ++address) {
            Require(ppc_read_u8(state, address) == 0, "Original ClearArena left nonzero arena bytes.");
        }
        Require(ppc_read_u8(state, 0x81700000u) == 0x6b,
                "Original ClearArena overran its exclusive upper bound.");
        for (unsigned index = 0; index < 128; ++index) {
            Require(ppc_read_u8(state, 0x81000000u + index) == 0,
                    "Original arena clearing did not include the former DSP bootstrap region.");
        }
        Require(state.dspControl == 0x804u && state.dspHardware->aramConfiguration == 0x43u &&
                    state.dspHardware->resetCount == 2 && state.dspHardware->dmaTransfers == 2 &&
                    state.dspHardware->dmaMainAddress == 0x01000000u &&
                    state.dspHardware->dmaAramAddress == 0 && state.dspHardware->dmaCount == 32 &&
                    state.dspHardware->toDspMailbox == 0 && state.dspHardware->fromDspMailbox == 0,
                "Original audio reset/DMA chain lost actual transfer evidence.");
        Require(state.dspHardware->programHalted && state.dspHardware->completedPrograms == 1 &&
                    state.dspHardware->sentMail == 1 && state.dspHardware->consumedMail == 1 &&
                    state.dspHardware->executedInstructions == 16404,
                "Original audio initialization did not execute and consume its compiled DSP program.");
        for (auto word : state.dspHardware->dataRam) {
            Require(word == 0, "Original compiled DSP program did not clear poisoned DSP RAM.");
        }
        auto audioReturn = state;
        audioReturn.lr = 0xfffffffcu;
        for (unsigned index = 0; index < 128; ++index) {
            ppc_write_u8(audioReturn, 0x81000000u + index, static_cast<uint8_t>(index ^ 0xa5u));
        }
        dispatcher.Execute(audioReturn, 0x801b20f4u);
        Require(audioReturn.pc == 0xfffffffcu && audioReturn.dspHardware->completedPrograms == 2,
                "Original audio initializer did not return through its caller after a real DSP program.");
        for (unsigned index = 0; index < 128; ++index) {
            Require(ppc_read_u8(audioReturn, 0x81000000u + index) == static_cast<uint8_t>(index ^ 0xa5u),
                    "Original audio initializer failed to restore its exact poisoned RAM backup.");
        }
        for (unsigned index = 0; index < state.dspHardware->aram.size(); ++index) {
            Require(state.dspHardware->aram[index] ==
                        (index < 32 ? ppc_read_u8(state, 0x802e0228u + index) : 0),
                    "Original audio ARAM DMA payload or full ARAM guard differs.");
        }
        auto noDspHardware = CreateOriginalStartupState(image);
        noDspHardware.dspHardware.reset();
        const auto dspFailure = RunOriginalStartup(dispatcher, noDspHardware);
        Require(dspFailure.pc == 0x801b2144u && dspFailure.reason.find("not mapped") != std::string::npos,
                "Missing DSP hardware profile was silently substituted.");
        auto noDspExecution = CreateOriginalStartupState(image);
        noDspExecution.executeDspInitialization = {};
        const auto noDspResult = RunOriginalStartup(dispatcher, noDspExecution);
        Require(noDspResult.pc == 0x801b223cu &&
                    noDspResult.reason.find("DSP unhalt requires") != std::string::npos &&
                    noDspExecution.dspHardware->sentMail == 0 &&
                    noDspExecution.dspHardware->completedPrograms == 0,
                "Absent DSP compiler backend was silently replaced by a ready response.");
        auto damagedDspCode = CreateOriginalStartupState(image);
        ppc_write_u16(damagedDspCode, 0x802e0228u, 0);
        const auto damagedDspResult = RunOriginalStartup(dispatcher, damagedDspCode);
        Require(damagedDspResult.pc == 0x801b223cu &&
                    damagedDspResult.reason.find("bootstrap payload differs") != std::string::npos &&
                    damagedDspCode.dspHardware->executedInstructions == 0 &&
                    damagedDspCode.dspHardware->sentMail == 0,
                "Modified DSP payload ran a stale compiled program or fabricated mail.");
        auto noMemoryProtection = CreateOriginalStartupState(image);
        noMemoryProtection.memoryProtection.reset();
        const auto noMemoryResult = RunOriginalStartup(dispatcher, noMemoryProtection);
        Require(noMemoryResult.pc == 0x801b5094u && noMemoryResult.reason.find("not mapped") != std::string::npos,
                "Missing memory-protection profile was silently substituted.");
        auto noRamBats = CreateOriginalStartupState(image);
        noRamBats.ramBats.reset();
        const auto noBatsResult = RunOriginalStartup(dispatcher, noRamBats);
        Require(noBatsResult.pc == 0x801b5068u && noBatsResult.reason.find("active system-call") != std::string::npos,
                "Missing RAM BAT profile silently skipped original real-mode configuration.");
        auto noColdReset = CreateOriginalStartupState(image);
        noColdReset.piResetCode.reset();
        const auto noResetResult = RunOriginalStartup(dispatcher, noColdReset);
        Require(noResetResult.pc == 0x801b5964u && noResetResult.reason.find("not mapped") != std::string::npos,
                "Missing PI reset code was silently replaced with a cold reset.");
        const auto sram = NewNativeSram();
        for (unsigned index = 0; index < sram.size(); ++index) {
            Require(ppc_read_u8(state, 0x80317b00u + index) == sram[index],
                    "Original SRAM initialization did not receive the exact native profile bytes.");
        }
        Require(ppc_read_u32(state, 0x80317b48u) == 0 &&
                    ppc_read_u32(state, 0x80317b4cu) == 1,
                "Original SRAM lock/sync state was not initialized successfully.");
        constexpr uint32_t thread = 0x80317f70u;
        Require(ppc_read_u32(state, 0x800000d4u) == thread &&
                    ppc_read_u32(state, 0x800000c0u) == (thread & 0x3fffffffu) &&
                    ppc_read_u32(state, 0x800000d8u) == 0 &&
                    ppc_read_u32(state, 0x800000dcu) == thread &&
                    ppc_read_u32(state, 0x800000e0u) == thread &&
                    ppc_read_u32(state, 0x800000e4u) == thread &&
                    ppc_read_u16(state, thread + 0x2c8u) == 2 &&
                    ppc_read_u16(state, thread + 0x2cau) == 1 &&
                    ppc_read_u32(state, thread + 0x2d0u) == 16 &&
                    ppc_read_u32(state, thread + 0x304u) == 0x8035cfa0u &&
                    ppc_read_u32(state, thread + 0x308u) == 0x8034cfa0u &&
                    ppc_read_u32(state, 0x8034cfa0u) == 0xdeadbabeu &&
                    ppc_read_msr(state) == 0x32u,
                "Original default thread, active queue, context ownership, stack guard or RI/FP state differs.");
        for (unsigned index = 0; index < 64; ++index) {
            Require(ppc_read_u32(state, 0x80317b58u + index * 4u) == 0,
                    "Original scheduler run queues were not initialized empty.");
        }
        auto missingDma = CreateOriginalStartupState(image);
        missingDma.transferExiDma = {};
        const auto dmaFailure = RunOriginalStartup(dispatcher, missingDma);
        Require(dmaFailure.pc == 0x801f66b0u &&
                    dmaFailure.reason.find("requires a native peripheral backend") != std::string::npos &&
                    missingDma.exiTransferControl[0] == 0x34u &&
                    ppc_read_u32(missingDma, 0x80317b4cu) == 0,
                "Missing SRAM DMA backend fabricated data or successful synchronization.");
        const auto profilePath = truncatedPath.parent_path() / "native-console.sram";
#ifdef _WIN32
        const auto persistedSram = LoadOrCreateNativeSram(profilePath);
        Require(persistedSram == sram && LoadOrCreateNativeSram(profilePath) == sram,
                "Fresh native SRAM profile was not persisted and reloaded exactly.");
#endif
        auto customSram = sram;
        customSram[0x20] = 0x7a;
        const auto customProfile = fixtures.Write("custom.sram", {customSram.begin(), customSram.end()});
        Require(LoadOrCreateNativeSram(customProfile) == customSram,
                "Existing native SRAM profile was reset instead of read unchanged.");
        const auto malformedProfile = fixtures.Write("malformed.sram", {1, 2, 3});
        RequireFailure([&] { LoadOrCreateNativeSram(malformedProfile); },
                       "Malformed native SRAM profile was silently replaced.");
        Require(std::filesystem::file_size(malformedProfile) == 3,
                "Rejected native SRAM profile was modified.");
        RequireFailure([&] { LoadOrCreateNativeSram(profilePath.parent_path() / "missing" / "console.sram"); },
                       "Unwritable native SRAM profile path was silently ignored.");
        const uint32_t expectedPolling =
            (uint32_t{ppc_read_u16(state, 0x802e60b8u)} << 16) |
            (uint32_t{ppc_read_u8(state, 0x802e60bau)} << 8);
        std::cout << "SI CSR=0x" << std::hex << state.serialInterface->communication
                  << " status=0x" << state.serialInterface->status << " active=0x"
                  << ppc_read_u32(state, 0x802e5fe4u) << std::dec << '\n';
        Require(state.piInterruptMask == 0xf8u &&
                    ppc_read_u32(state, 0x80003040u + 20u * 4u) == 0x801f4d58u &&
                    state.serialInterface->polling == expectedPolling &&
                    state.serialInterface->communication == 0x60010306u &&
                    state.serialInterface->status == 0 &&
                    ppc_read_u32(state, 0x802e5fe4u) == UINT32_MAX &&
                    ppc_read_u32(state, 0x80340a60u) == UINT32_MAX,
                "Original SI sampling/interrupt setup differs or a controller completion was fabricated.");
        for (uint32_t channel = 0; channel < 4; ++channel) {
            Require(ppc_read_u32(state, 0x80340a60u + channel * 0x20u) == UINT32_MAX &&
                        (ppc_read_u32(state, 0x802e5ff8u + channel * 4u) & 8u) != 0,
                    "Original SI queues did not drain or retain disconnected-controller results.");
        }
        auto noInterruptDelivery = CreateOriginalStartupState(image);
        noInterruptDelivery.externalInterrupt.reset();
        const auto interruptFailure = RunOriginalStartup(dispatcher, noInterruptDelivery);
        Require(interruptFailure.pc == 0x801b3a60u &&
                    interruptFailure.reason.find("requires guest interrupt delivery") != std::string::npos &&
                    !noInterruptDelivery.externalInterruptsEnabled,
                "Missing interrupt backend discarded a real pending SI completion.");
        auto resumedInterrupts = noInterruptDelivery;
        resumedInterrupts.externalInterrupt.emplace();
        resumedInterrupts.externalInterruptsEnabled = true;
        resumedInterrupts.lr = 0xfffffffcu;
        resumedInterrupts.cr = 0x12345678u;
        resumedInterrupts.xer = 0xe0000000u;
        resumedInterrupts.ctr = 0x87654321u;
        for (unsigned reg = 0; reg < 32; ++reg) {
            if (reg != 1 && reg != 2 && reg != 13) {
                resumedInterrupts.gpr[reg] = 0xa5000000u + reg;
            }
        }
        auto uninterrupted = resumedInterrupts;
        uninterrupted.piInterruptMask = 0;
        dispatcher.Execute(uninterrupted, 0x801b3a58u);
        dispatcher.Execute(resumedInterrupts, 0x801b3a58u);
        Require(std::equal(std::begin(uninterrupted.gpr), std::end(uninterrupted.gpr),
                           std::begin(resumedInterrupts.gpr)) &&
                    uninterrupted.cr == resumedInterrupts.cr && uninterrupted.xer == resumedInterrupts.xer &&
                    uninterrupted.ctr == resumedInterrupts.ctr && uninterrupted.lr == resumedInterrupts.lr &&
                    uninterrupted.gqr == resumedInterrupts.gqr && uninterrupted.fpr == resumedInterrupts.fpr &&
                    uninterrupted.fpscr == resumedInterrupts.fpscr &&
                    ppc_read_msr(uninterrupted) == ppc_read_msr(resumedInterrupts) &&
                    resumedInterrupts.externalInterrupt->returned == 4,
                "Original exception vector/OSLoadContext did not preserve every interrupted CPU register.");
        auto corruptedVector = noInterruptDelivery;
        corruptedVector.externalInterrupt.emplace();
        corruptedVector.externalInterruptsEnabled = true;
        corruptedVector.lr = 0xfffffffcu;
        ppc_write_u32(corruptedVector, 0x568u, 0x38600005u);
        RequireFailure([&] { dispatcher.Execute(corruptedVector, 0x801b3a58u); },
                       "Execution accepted an incorrectly patched original external interrupt vector.");
        Require(corruptedVector.externalInterrupt->returned == 0 &&
                    !corruptedVector.callGuest && !corruptedVector.callException,
                "Vector integrity failure fabricated return or leaked dispatcher callbacks.");
        auto corruptedTemplate = CreateOriginalStartupState(image);
        ppc_write_u32(corruptedTemplate, 0x801b14f8u, 0x38600004u);
        NativeDispatcher corruptSource;
        RequireFailure([&] { RegisterOriginalStartup(corruptSource, corruptedTemplate); },
                       "Patched-vector registration did not verify the unpatched DOL source.");
        auto missingSiBackend = CreateOriginalStartupState(image);
        missingSiBackend.transferSi = {};
        const auto siFailure = RunOriginalStartup(dispatcher, missingSiBackend);
        Require(siFailure.pc == 0x801f5578u &&
                    siFailure.reason.find("controller transaction/completion backend") != std::string::npos &&
                    missingSiBackend.serialInterface->communication == 0 &&
                    missingSiBackend.serialInterface->status == 0,
                "Missing SI backend fabricated a response or completion.");
        auto missingDtv = CreateOriginalStartupState(image);
        missingDtv.viDtvStatus.reset();
        missingDtv.video.reset();
        const auto dtvFailure = RunOriginalStartup(dispatcher, missingDtv);
        Require(dtvFailure.pc == 0x801f6180u &&
                    dtvFailure.reason.find("not mapped") != std::string::npos,
                "SI sampling silently substituted a missing VI cable-status profile.");
        auto noConsole = CreateOriginalStartupState(image);
        noConsole.writeConsole = {};
        const auto consoleFailure = RunOriginalStartup(dispatcher, noConsole);
        Require(consoleFailure.pc == 0x802383c0u &&
                    consoleFailure.reason.find("requires a native console sink") != std::string::npos,
                "Original version logging silently ignored a missing native console.");
        Require(!state.callGuest && state.hid0 == 0xc200u && state.hid2 == 0xa0000000u &&
                    state.l2cr == 0x80000000u && !state.externalInterruptsEnabled &&
                    ppc_read_u32(state, 0x80349904u) == 1u &&
                    ppc_read_u32(state, 0x80003010u) == 0x801b4260u &&
                    ppc_read_u32(state, 0x8000301cu) == 0x801b3274u,
                "Real original startup did not preserve its hardware/OS initialization evidence.");
        RequireFailure([&] { RunOriginalStartup(dispatcher, state); },
                       "Runner accepted reuse of an incomplete startup state.");

        auto limited = CreateOriginalStartupState(image);
        const auto budget = RunOriginalStartup(dispatcher, limited, 1);
        Require(budget.stop == OriginalStartupStop::Budget && !limited.callGuest,
                "Block budget did not stop execution and restore callbacks.");
        auto shallow = CreateOriginalStartupState(image);
        Require(RunOriginalStartup(dispatcher, shallow, 1000000, 1).stop ==
                    OriginalStartupStop::Budget,
                "Call-depth bound was not enforced.");
        auto changed = CreateOriginalStartupState(image);
        changed.memory[ppc_memory_offset(changed, image.entryPoint(), 1)] ^= 1u;
        NativeDispatcher rejected;
        RequireFailure([&] { RegisterOriginalStartup(rejected, changed); },
                       "Registration accepted changed original instructions.");
        const auto mismatch = RunOriginalStartup(dispatcher, changed);
        Require(mismatch.stop == OriginalStartupStop::Dependency &&
                    mismatch.reason.find("code mismatch") != std::string::npos,
                "Execution accepted changed original instructions.");
        auto unavailable = CreateOriginalStartupState(image);
        unavailable.readTimeBase = {};
        const auto missingClock = RunOriginalStartup(dispatcher, unavailable);
        Require(missingClock.stop == OriginalStartupStop::Dependency &&
                    missingClock.reason.find("time") != std::string::npos,
                "Missing time-base dependency was silently emulated.");
        // Force the original upper/lower/upper sampling loop never to converge.
        auto looping = CreateOriginalStartupState(image);
        uint64_t samples = 0;
        looping.readTimeBase = [&] { return ++samples << 32; };
        bool exhausted = false;
        try {
            dispatcher.Execute(looping, 0x801b7bf0u, 16);
        } catch (const std::runtime_error& error) {
            exhausted = std::string(error.what()).find("budget exceeded") != std::string::npos;
        }
        Require(exhausted && samples == 48 && looping.pc == 0x801b7bf0u &&
                    !looping.callGuest,
                "Original nonconverging OSGetTime loop bypassed the block budget.");
        if (argc == 4) {
            RvzImage disc(argv[3]);
            {
                auto missingGraphics = CreateOriginalStartupState(image);
                missingGraphics.graphics.reset();
                InstallOriginalStartupDisc(missingGraphics, disc);
                uint64_t clock = 1000000;
                missingGraphics.readTimeBase = [&] { return clock += 1000; };
                missingGraphics.writeConsole = [](const std::string&) {};
                const auto missing = RunOriginalStartup(dispatcher, missingGraphics);
                Require(missing.pc == 0x801b3e00u && missing.gxInitEntered &&
                            missing.reason.find("Unsupported PI interrupt mask") != std::string::npos &&
                            missingGraphics.piInterruptMask == 0xfcu,
                        "Missing graphics backend did not retain the explicit original CP-enable boundary.");
            }
            auto discState = CreateOriginalStartupState(image);
            const auto originalMemory = discState.memory;
            InstallOriginalStartupDisc(discState, disc);
            const auto fstAddress = ppc_read_u32(discState, 0x80000038u);
            const auto fstSize = ppc_read_u32(discState, 0x8000003cu);
            const auto fstOffset = ppc_memory_offset(discState, fstAddress, fstSize);
            const std::vector<uint8_t> fst(discState.memory.begin() + fstOffset,
                                         discState.memory.begin() + fstOffset + fstSize);
            Require(fstAddress == 0x81702000u && fstSize != 0 &&
                        ppc_read_u32(discState, 0x800000f4u) == 0x81700000u &&
                        ppc_read_u32(discState, 0x80000020u) == 0x0d15ea5eu &&
                        std::equal(originalMemory.begin() + 0x3100, originalMemory.begin() + 0x35efa0,
                                   discState.memory.begin() + 0x3100),
                    "Native RVZ loader metadata overwrote original code, data or BSS.");
            RequireFailure([&] { InstallOriginalStartupDisc(discState, disc); },
                           "Native disc installation accepted an already bound state.");
            Require(discState.transferDisc({0xa8000040u, 0, 32}, 32) == disc.Read(0, 32) &&
                        discState.transferDisc({0xa8000000u, 0x108, 32}, 32) == disc.Read(0x420, 32),
                    "Native DI read-ID or normal read did not return actual RVZ bytes.");
            RequireFailure([&] { discState.transferDisc({0xa8000000u, UINT32_MAX, 32}, 32); },
                           "Native RVZ backend accepted an out-of-range read.");
            RequireFailure([&] { discState.transferDisc({0xa8000000u, 0, 64}, 32); },
                           "Native RVZ backend accepted mismatched command/DMA lengths.");
            RequireFailure([&] { discState.transferDisc({0xff000000u, 0, 32}, 32); },
                           "Native RVZ backend silently accepted an unsupported command.");
            uint64_t ticks = 1000000;
            discState.readTimeBase = [&] { return ticks += 1000; };
            std::string discConsole;
            discState.writeConsole = [&](const std::string& text) { discConsole += text; };
            const auto discResult = RunOriginalStartup(dispatcher, discState);
            std::cout << DescribeOriginalStartupStop(discResult) << '\n';
            std::cout << "Original VI startup: PI mask=0x" << std::hex << discState.piInterruptMask.value()
                      << std::dec << " interrupts=" << discState.externalInterrupt->delivered
                      << "/" << discState.externalInterrupt->returned
                      << " EE=" << discState.externalInterruptsEnabled
                      << " thread returns=" << discState.threadContextReturns
                      << " DI transfers=" << discState.discInterface->completedTransfers
                      << " VI fields=" << discState.video->fields << '\n';
            Require(discResult.stop == OriginalStartupStop::Dependency &&
                        discResult.pc == 0x801b8ae0u && discResult.lr == 0x8020ad8cu &&
                        discResult.stack == 0x8035cea8u && discResult.osInitReturned &&
                        discResult.constructorsReturned == 93 && discResult.userInitReturned &&
                        discResult.gameMainEntered && discResult.memoryInitialized &&
                        discResult.screenInitEntered && discResult.gxInitEntered &&
                        discResult.gxInitReturned && discResult.viInitEntered &&
                        discResult.viInitReturned && discResult.viConfigured && discResult.viRetraceReturned &&
                        discResult.screenInitReturned &&
                        !discState.callGuest && !discState.callException &&
                        discResult.reason == "No native translation registered at 0x801B8AE0." &&
                        discState.discInterface->completedTransfers == 1 &&
                        discState.discInterface->status == 0x2au &&
                        discState.discInterface->dmaLength == 0 &&
                        discState.discInterface->control == 2 &&
                        discState.externalInterrupt->delivered == 162 &&
                        discState.externalInterrupt->returned == 162 && discState.threadContextReturns == 1 &&
                        !discState.externalInterrupt->active && discState.externalInterruptsEnabled &&
                        discState.piInterruptMask == 0xffcu &&
                        ppc_read_u16(discState, 0x800030e6u) == 0x8001u &&
                        ppc_read_u32(discState, 0x80317a6cu) == 0 &&
                        ppc_read_u32(discState, 0x80349698u) == 0 &&
                        ppc_read_u32(discState, 0x803185b8u) == 0 &&
                        ppc_read_u32(discState, 0x803495fcu) != 0 &&
                        discConsole.find("SDK - DVD") != std::string::npos &&
                        discConsole.find("SDK - GX") != std::string::npos &&
                        discConsole.find("SDK - VI") != std::string::npos &&
                        DescribeOriginalStartupStop(discResult).find("GXInit returned") != std::string::npos &&
                        DescribeOriginalStartupStop(discResult).find("OSInit returned") != std::string::npos &&
                        std::equal(fst.begin(), fst.end(), discState.memory.begin() + fstOffset),
                    "Original entry did not reach original graphics setup through constructors, heaps and real DVD DMA.");
            const auto& gx = *discState.graphics;
            const auto fifo = ppc_read_u32(discState, ppc_read_u32(discState, 0x803491b4u) + 12) & 0x3fffffffu;
            std::cout << "Original GX bursts=" << gx.bursts << " commands=" << gx.decodedCommands
                      << " CP control=" << gx.control << " PE control=" << gx.peControl << '\n';
            std::cout << "PE poke:";
            for (const auto value : gx.poke) std::cout << " " << std::hex << value;
            std::cout << std::dec << " metric resets=" << gx.performanceResets
                      << " interval bursts/commands=" << gx.performanceBursts << "/" << gx.performanceCommands << '\n';
            Require(gx.bursts == 77 && gx.decodedCommands == 404 &&
                        gx.gathered.size() == 15 && gx.commands.empty() &&
                        gx.cpuBase == fifo && gx.cpuEnd == fifo + 0x5ffe0 &&
                        gx.cpuWrite == fifo + 77 * 32 && gx.fifo[0] == gx.cpuBase &&
                        gx.fifo[1] == gx.cpuEnd && gx.fifo[4] == 0 &&
                        gx.fifo[5] == gx.cpuWrite && gx.fifo[6] == gx.cpuWrite &&
                        gx.writeGatherAddress == 0x0c008000u && (discState.hid2 & 0x40000000u) != 0 &&
                        gx.peControl == 3 && gx.performanceResets == 1 &&
                        gx.performanceBursts == 14 && gx.performanceCommands == 89 &&
                        gx.poke == std::array<uint16_t, 5>{0x1f, 0xf038, 0, 0x700, 5} &&
                        NativeGxInterruptCause(gx) == 0,
                    "Original GX FIFO/PE setup did not consume the exact actual command bursts.");
            Require(gx.efb && gx.efb->copies == 1 && gx.efb->clears == 1 &&
                        gx.efb->undefinedCopies == 1 && gx.efb->lastCopy &&
                        gx.efb->lastCopy->address == 0x0157b160u &&
                        gx.efb->lastCopy->width == 640 && gx.efb->lastCopy->height == 448 &&
                        gx.efb->lastCopy->stride == 1280 &&
                        gx.efb->lastCopy->undefinedPixels == 640 * 448 &&
                        (ppc_read_u32(discState, ppc_read_u32(discState, 0x803491b4u) + 4) & 0x3fffffffu) ==
                            gx.efb->lastCopy->address,
                    "Original Screen did not copy its unspecified power-on EFB into the actual second XFB before clearing.");
            for (uint32_t pixel = 0; pixel < NativeEfb::width * NativeEfb::height; ++pixel) {
                const bool cleared = pixel < 640 * 448;
                Require(gx.efb->color[pixel] == (cleared ? 0x404040ffu : 0xcdcdcdffu) &&
                            gx.efb->depth[pixel] == (cleared ? 0xffffffu : 0xcdcdcdu) &&
                            gx.efb->defined[pixel] == (cleared ? 3 : 0),
                        "Original EFB clear changed the wrong rectangle, masks, RGB/depth or defined-pixel state.");
            }
            for (uint32_t offset = 0; offset < 640 * 448 * 2; offset += 4) {
                Require(ppc_read_u32(discState, gx.efb->lastCopy->address + offset) == 0xc080c080u,
                        "Original XFB RAM does not contain the documented representative of undefined power-on pixels.");
            }
            const uint32_t viewport[] = {0x43a00000, 0xc3600000, 0x4b7fc000,
                                         0x44258000, 0x440d8000, 0x4b800000};
            std::cout << "XF viewport:";
            for (unsigned index = 0; index < 6; ++index) std::cout << " " << std::hex << gx.xf[0x101a + index];
            std::cout << std::dec << '\n';
            Require(std::equal(std::begin(viewport), std::end(viewport), gx.xf.begin() + 0x101a),
                    "Original floating-point viewport did not publish exact overscanned 640x448 XF coefficients.");
            for (uint32_t address = 0x80317a40u; address < 0x80317a60u; ++address) {
                Require(ppc_read_u8(discState, address) == (address == 0x80317a43u ? 1 : 0),
                        "Native virtual-drive identity differs or claims console firmware.");
            }
            Require(discState.decrementer->armed && discState.decrementer->writes == 1 &&
                        ticks < discState.decrementer->deadline,
                    "Original DVD alarm did not program an actual future decrementer deadline.");

            auto traced = CreateOriginalStartupState(image);
            InstallOriginalStartupDisc(traced, disc);
            uint64_t tracedTicks = 1000000;
            traced.readTimeBase = [&] { return tracedTicks += 1000; };
            std::string tracedConsole;
            traced.writeConsole = [&](const std::string& text) { tracedConsole += text; };
            std::vector<uint32_t> constructors;
            for (uint32_t offset = 0; offset < 93 * 4; offset += 4) {
                constructors.push_back(ppc_read_u32(traced, 0x8023dd80u + offset));
            }
            Require(ppc_read_u32(traced, 0x8023dd80u + 93 * 4) == 0,
                    "Original constructor table does not have exactly 93 entries.");
            size_t entered = 0, returned = 0;
            bool initializedHeap = false;
            bool gxReturned = false;
            bool viReturned = false, viConfigured = false, retraceReturned = false, screenReturned = false;
            unsigned viHandlers = 0;
            std::optional<PpcState> peSource;
            uint64_t peSourceTicks = 0;
            std::vector<std::pair<uint32_t, uint32_t>> allocations;
            std::vector<std::pair<uint32_t, uint32_t>> allAllocations;
            std::optional<std::pair<uint32_t, uint32_t>> allocationRequest;
            std::array<uint32_t, 32> savedRegisters{};
            std::array<uint64_t, 18> savedFprs{};
            const auto observe = [&](uint32_t pc) {
                if (allocationRequest && pc == allocationRequest->first) {
                    const auto address = traced.gpr[3], size = allocationRequest->second;
                    Require(initializedHeap && size != 0 && (address & 31u) == 0 &&
                                address >= 0x8035efa0u && uint64_t(address) + size <= 0x81701fe0u &&
                                std::all_of(allAllocations.begin(), allAllocations.end(), [&](const auto& previous) {
                                    return uint64_t(address) + size <= previous.first ||
                                           uint64_t(previous.first) + previous.second <= address;
                                }),
                            "Original allocation returned overlapping, unaligned or out-of-arena memory.");
                    allAllocations.emplace_back(address, size);
                    allocationRequest.reset();
                }
                if (pc == 0x80178e88u || pc == 0x80178ff8u || pc == 0x801786d4u) {
                    Require(!allocationRequest, "Original allocator unexpectedly nested an allocation.");
                    allocationRequest = std::make_pair(traced.lr, traced.gpr[3]);
                }
                if (pc == 0x801b7cc0u) {
                    Require(entered == returned && entered < constructors.size() &&
                                traced.gpr[12] == constructors[entered] &&
                                traced.gpr[31] == 0x8023dd80u + entered * 4,
                            "Original constructors were skipped, reordered or replayed.");
                    std::copy(std::begin(traced.gpr), std::end(traced.gpr), savedRegisters.begin());
                    for (unsigned reg = 14; reg < 32; ++reg) savedFprs[reg - 14] = traced.fpr[reg].ps0;
                    ++entered;
                } else if (pc == 0x801b7cc8u) {
                    Require(entered == returned + 1 && traced.gpr[1] == savedRegisters[1] &&
                                traced.gpr[2] == savedRegisters[2] && traced.gpr[13] == savedRegisters[13],
                            "Original constructor did not restore its stack or SDA registers.");
                    for (unsigned reg = 14; reg < 32; ++reg) {
                        Require(traced.gpr[reg] == savedRegisters[reg] &&
                                    traced.fpr[reg].ps0 == savedFprs[reg - 14],
                                "Original constructor corrupted nonvolatile ABI registers.");
                    }
                    ++returned;
                } else if (pc == 0x800032a0u || pc == 0x8000aa54u) {
                    Require(entered == 93 && returned == 93 && traced.gpr[1] == 0x8035cf98u,
                            "User initialization/game entry preceded complete constructor return.");
                } else if (pc == 0x8017a41cu) {
                    std::cout << "GXInit return SP=" << std::hex << traced.gpr[1]
                              << " FIFO=" << traced.gpr[3] << std::dec << '\n';
                    Require(!gxReturned && traced.gpr[1] == 0x8035cf28u &&
                                traced.gpr[3] == 0x80318b20u,
                            "Original GXInit did not return its FIFO object and restore the Screen stack.");
                    gxReturned = true;
                    peSource = traced;
                    peSource->callGuest = {};
                    peSource->callException = {};
                    peSourceTicks = tracedTicks;
                } else if (pc == 0x8017a42cu) {
                    Require(gxReturned && traced.gpr[1] == 0x8035cf28u,
                            "Original VIInit did not restore the Screen stack.");
                    viReturned = true;
                } else if (pc == 0x8017a4c0u) {
                    Require(viReturned, "VIConfigure returned before VIInit.");
                    viConfigured = true;
                } else if (pc == 0x801bde64u) {
                    ++viHandlers;
                } else if (pc == 0x8017a674u) {
                    Require(viConfigured && viHandlers == 2 && traced.threadContextReturns == 1,
                            "VIWaitForRetrace did not use the original VI interrupt and scheduler context restore.");
                    retraceReturned = true;
                } else if (pc == 0x8000addcu) {
                    std::cout << "Screen return SP=" << std::hex << traced.gpr[1] << std::dec
                              << " allocations=" << ppc_read_u32(traced, 0x80349494u) << '\n';
                    Require(retraceReturned && traced.gpr[1] == 0x8035cf68u &&
                                ppc_read_u32(traced, 0x80349494u) == 11,
                            "Original Screen constructor did not return after retrace and eleven real allocations.");
                    screenReturned = true;
                } else if (pc == 0x8000ad5cu) {
                    const uint32_t size = (0x81701fe0u - 0x8035efa0u) & ~31u;
                    Require(ppc_read_u32(traced, 0x80000030u) == 0x8035efa0u &&
                                ppc_read_u32(traced, 0x80315508u) == 0x81701fe0u &&
                                ppc_read_u32(traced, 0x8031550cu) == size &&
                                ppc_read_u32(traced, 0x80315510u) == 0x802fd4fcu &&
                                ppc_read_u16(traced, 0x80315514u) == 4096 &&
                                ppc_read_u32(traced, 0x802fd4fcu) == 0x81701fe0u &&
                                ppc_read_u32(traced, 0x802fd500u) == size &&
                                ppc_read_u32(traced, 0x80349494u) == 0,
                            "Original arena-backed heap initialization differs from actual loader bounds.");
                    initializedHeap = true;
                } else {
                    uint32_t size = 0;
                    switch (pc) {
                    case 0x8000ad70u: size = 12; break;
                    case 0x8000ad90u: size = 4; break;
                    case 0x8019023cu: size = 0x4b10; break;
                    case 0x80199318u: size = 0x1730; break;
                    case 0x80190260u: size = 0x8a10; break;
                    case 0x80190280u: size = 0x44; break;
                    case 0x8000adb0u: size = 0xa0; break;
                    case 0x8000add0u: size = 0xac; break;
                    case 0x8017a40cu: size = 0x60000; break;
                    case 0x8017a634u:
                    case 0x8017a64cu: size = 640 * 448 * 2; break;
                    }
                    if (size != 0) {
                        const auto address = traced.gpr[3];
                        Require(initializedHeap && address >= 0x8035efa0u &&
                                    address + size <= 0x81701fe0u && (address & 31u) == 0 &&
                                    (allocations.empty() ||
                                     address + size <= allocations.back().first),
                                "Original initialization allocation failed, overlapped or escaped the arena.");
                        allocations.emplace_back(address, size);
                    }
                }
            };
            bool reachedBoundary = false;
            try {
                dispatcher.Execute(traced, image.entryPoint(), 1000000, 128, observe);
            } catch (const std::exception& error) {
                reachedBoundary = traced.pc == 0x801b8ae0u &&
                    std::string(error.what()) == "No native translation registered at 0x801B8AE0.";
                if (!reachedBoundary) throw;
            }
            std::cout << "Observed original initialization allocations=" << allocations.size()
                      << ", all returned allocations=" << allAllocations.size()
                      << ", allocator count=" << ppc_read_u32(traced, 0x80349494u) << '\n';
            Require(initializedHeap && allocations.size() == 11,
                    "Original initialization did not return all eleven observed heap/framebuffer allocations.");
            Require(!allocationRequest && allAllocations.size() == 39 &&
                        std::equal(allocations.begin(), allocations.end(), allAllocations.begin()) &&
                        ppc_read_u32(traced, 0x80349494u) == allAllocations.size(),
                    "Original allocation counter/returned requests differ from observed allocations.");
            Require(ppc_read_u32(traced, 0x8034915cu) == allocations[0].first &&
                        ppc_read_u32(traced, 0x8034919cu) == allocations[1].first &&
                        ppc_read_u32(traced, 0x803491b4u) == allocations[7].first &&
                        ppc_read_u32(traced, allocations[7].first + 12) == allocations[8].first &&
                        ppc_read_u32(traced, allocations[7].first) == allocations[9].first &&
                        ppc_read_u32(traced, allocations[7].first + 4) == allocations[10].first &&
                        ppc_read_u32(traced, allocations[7].first + 8) == allocations[10].first,
                    "Original manager singletons or Screen FIFO pointer differ from allocated objects.");
            Require(reachedBoundary && gxReturned && viReturned && viConfigured && retraceReturned && screenReturned &&
                        entered == 93 && returned == 93 &&
                        traced.memory == discState.memory && tracedConsole == discConsole &&
                        tracedTicks == ticks && traced.pc == discState.pc && traced.lr == discState.lr &&
                        std::equal(std::begin(traced.gpr), std::end(traced.gpr), std::begin(discState.gpr)) &&
                        traced.fpr == discState.fpr && traced.fpscr == discState.fpscr &&
                        traced.cr == discState.cr && traced.xer == discState.xer &&
                        traced.ctr == discState.ctr && traced.gqr == discState.gqr &&
                        ppc_read_msr(traced) == ppc_read_msr(discState) &&
                        !traced.callGuest && !traced.callException,
                    "Observed startup changed original initialization, memory, registers or callback cleanup.");
            const auto& observedGx = *traced.graphics;
            Require(observedGx.efb && observedGx.efb->lastCopy &&
                        observedGx.efb->color == gx.efb->color &&
                        observedGx.efb->depth == gx.efb->depth &&
                        observedGx.efb->defined == gx.efb->defined &&
                        observedGx.efb->copies == gx.efb->copies &&
                        observedGx.efb->clears == gx.efb->clears &&
                        observedGx.efb->undefinedCopies == gx.efb->undefinedCopies &&
                        observedGx.efb->lastCopy->address == gx.efb->lastCopy->address &&
                        observedGx.efb->lastCopy->width == gx.efb->lastCopy->width &&
                        observedGx.efb->lastCopy->height == gx.efb->lastCopy->height &&
                        observedGx.efb->lastCopy->stride == gx.efb->lastCopy->stride &&
                        observedGx.efb->lastCopy->undefinedPixels == gx.efb->lastCopy->undefinedPixels,
                    "Observer changed EFB storage, clear/copy effects or undefined-pixel provenance.");
            const auto& vi = *discState.video;
            const auto& observedVi = *traced.video;
            Require(vi.resets == 1 && vi.fields == 152 && viHandlers == 152 && vi.clockStarted &&
                        vi.registers[1] == 1 && vi.registers[3] == 429 &&
                        NativeViInterruptCause(vi) == 0 &&
                        observedVi.registers == vi.registers && observedVi.lastTicks == vi.lastTicks &&
                        observedVi.elapsedTicks == vi.elapsedTicks && observedVi.fields == vi.fields &&
                        observedVi.writes == vi.writes && observedVi.resets == vi.resets &&
                        observedVi.clockStarted == vi.clockStarted &&
                        traced.threadContextReturns == discState.threadContextReturns &&
                        traced.externalInterrupt->delivered == discState.externalInterrupt->delivered &&
                        traced.externalInterrupt->returned == discState.externalInterrupt->returned,
                    "Observed execution changed actual VI timing/register/interrupt or original scheduler state.");
            Require(observedGx.control == gx.control && observedGx.peControl == gx.peControl &&
                        observedGx.pePending == gx.pePending &&
                        observedGx.fifo == gx.fifo && observedGx.cp == gx.cp &&
                        observedGx.bp == gx.bp && observedGx.xf == gx.xf &&
                        observedGx.gathered == gx.gathered && observedGx.commands == gx.commands &&
                        observedGx.bpMask == gx.bpMask && observedGx.cpuBase == gx.cpuBase &&
                        observedGx.cpuEnd == gx.cpuEnd && observedGx.cpuWrite == gx.cpuWrite &&
                        observedGx.writeGatherAddress == gx.writeGatherAddress &&
                        observedGx.watermarkStatus == gx.watermarkStatus &&
                        observedGx.cpToken == gx.cpToken && observedGx.token == gx.token &&
                        observedGx.performanceSelect == gx.performanceSelect &&
                        observedGx.performanceBursts == gx.performanceBursts &&
                        observedGx.performanceCommands == gx.performanceCommands &&
                        observedGx.performanceResets == gx.performanceResets && observedGx.poke == gx.poke &&
                        observedGx.bursts == gx.bursts && observedGx.decodedCommands == gx.decodedCommands &&
                        observedGx.vertexCacheInvalidations == gx.vertexCacheInvalidations,
                    "Observed execution changed actual GX command, FIFO or interrupt state.");
            {
                Require(peSource.has_value(), "Missing pre-VI original GX return snapshot.");
                const auto& interrupted = *peSource;
                auto pe = interrupted;
                uint64_t peTicks = peSourceTicks;
                pe.readTimeBase = [&] { return peTicks += 1000; };
                const auto padding = 32 - pe.graphics->gathered.size();
                for (size_t byte = 0; byte < padding; ++byte) ppc_write_u8(pe, 0xcc008000u, 0);
                Require(pe.graphics->commands.empty() && pe.graphics->gathered.empty(),
                        "Original pending matrix command did not finish on an actual gather flush.");
                const uint8_t requests[] = {0x61, 0x48, 0, 0x12, 0x34, 0x61, 0x45, 0, 0, 2};
                for (const auto byte : requests) ppc_write_u8(pe, 0xcc008000u, byte);
                for (unsigned byte = 10; byte < 32; ++byte) ppc_write_u8(pe, 0xcc008000u, 0);
                Require(NativeGxInterruptCause(*pe.graphics) == 0x600u &&
                            ppc_read_u16(pe, 0xcc00100au) == 3,
                        "Test FIFO token/finish commands did not assert both real PE sources.");
                unsigned tokenHandlers = 0, finishHandlers = 0;
                bool resumed = false;
                try {
                    dispatcher.Execute(pe, interrupted.pc, 100000, 128, [&](uint32_t pc) {
                        if (pc == 0x801ce08cu) ++tokenHandlers;
                        if (pc == 0x801ce158u) ++finishHandlers;
                        if (pc == interrupted.pc) throw std::runtime_error("PE test context resumed.");
                    });
                } catch (const std::exception& error) {
                    resumed = pe.pc == interrupted.pc &&
                        std::string(error.what()) == "PE test context resumed.";
                    if (!resumed) throw;
                }
                Require(resumed && tokenHandlers == 1 && finishHandlers == 1 &&
                            pe.externalInterrupt->delivered == 7 && pe.externalInterrupt->returned == 7 &&
                            !pe.externalInterrupt->active && NativeGxInterruptCause(*pe.graphics) == 0 &&
                            pe.graphics->peControl == 3 && pe.graphics->token == 0x1234 &&
                            ppc_read_u8(pe, 0x803498e8u) == 1 &&
                            std::equal(std::begin(pe.gpr), std::end(pe.gpr), std::begin(interrupted.gpr)) &&
                            pe.fpr == interrupted.fpr && pe.cr == interrupted.cr && pe.xer == interrupted.xer &&
                            pe.lr == interrupted.lr && pe.ctr == interrupted.ctr &&
                            ppc_read_msr(pe) == ppc_read_msr(interrupted) && !pe.callGuest && !pe.callException,
                        "Original PE handlers did not acknowledge both sources and restore interrupted context.");
                std::cout << "Injected FIFO token/finish requests executed both original PE handlers "
                             "and original exception/context return. Production startup issues no such requests.\n";
            }
            auto damagedConstructor = CreateOriginalStartupState(image);
            InstallOriginalStartupDisc(damagedConstructor, disc);
            damagedConstructor.writeConsole = [](const std::string&) {};
            ppc_write_u32(damagedConstructor, constructors[11], 0);
            const auto damagedResult = RunOriginalStartup(dispatcher, damagedConstructor);
            Require(damagedResult.stop == OriginalStartupStop::Dependency &&
                        damagedResult.osInitReturned && damagedResult.constructorsReturned == 11 &&
                        !damagedResult.userInitReturned && !damagedResult.gameMainEntered &&
                        damagedResult.reason.find("code mismatch") != std::string::npos &&
                        !damagedConstructor.callGuest && !damagedConstructor.callException,
                    "Damaged constructor code bypassed integrity checks or reported a false startup milestone.");
            auto absentBackend = CreateOriginalStartupState(image);
            InstallOriginalStartupDisc(absentBackend, disc);
            absentBackend.transferDisc = {};
            const auto missingDiscTransfer = RunOriginalStartup(dispatcher, absentBackend);
            Require(missingDiscTransfer.pc == 0x801b9fd0u &&
                        missingDiscTransfer.reason.find("requires a native disc transfer backend") != std::string::npos &&
                        absentBackend.discInterface->completedTransfers == 0 &&
                        absentBackend.discInterface->dmaLength == 32 &&
                        ppc_read_u16(absentBackend, 0x800030e6u) == 0,
                    "Missing native disc backend fabricated inquiry DMA or OSInit completion.");
        }
        fixtures.Cleanup();
        std::cout << "Production original-startup binding, real entry/OSInit boundary, "
                     "dispatch/loop limits, disk/RAM integrity and missing-dependency checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "Original startup test failure: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
