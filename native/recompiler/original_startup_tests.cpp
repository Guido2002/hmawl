#include "original_startup.h"
#include "native_console.h"
#include "native_exi_bus.h"
#include "native_si_bus.h"
#include "native_sram.h"

#include <chrono>
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
        Require(argc == 2, "Expected verified USA DOL.");
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
            Require(request.channel == 0 && request.output == std::vector<uint8_t>{0} &&
                        request.inputLength == 3 && siTransfers++ == 0,
                    "Original SI controller type query differs or an unserviced queue was bypassed.");
            return TransferDisconnectedSi(request);
        };
        state.writeConsole = [&](const std::string& text) { console += text; };
        const auto expectedVersion = ReadGuestConsoleString(state, 0x802e6150u) + '\n' +
            ReadGuestConsoleString(state, ppc_read_u32(state, 0x80348cd8u)) + '\n';
        const auto result = RunOriginalStartup(dispatcher, state);
        std::cout << DescribeOriginalStartupStop(result) << '\n';
        Require(result.stop == OriginalStartupStop::Dependency && result.pc == 0x801b223cu &&
                    result.lr == 0x801b21e0u && transfers == 2 && siTransfers == 1 &&
                    sramCommands == 1 && sramDmas == 1 &&
                    console == expectedVersion && state.exiStatus[0] == 0x2008u &&
                    state.exiTransferControl[0] == 2u &&
                    ppc_read_u32(state, 0x80340c6cu) == 0 &&
                    ppc_read_u32(state, 0x800030c0u) == 0 &&
                    ppc_read_u32(state, 0x800030c4u) == 0 &&
                    result.reason.find("DSP unhalt requires a native DSP execution backend") != std::string::npos &&
                    ppc_read_u32(state, 0x80349ad0u) == 0,
                "Production startup stopped at an unexpected boundary.");
        Require(state.srr0 == 0x801b23f4u && state.srr1 == 0x32u &&
                    !state.systemCallActive && !state.callException && state.hid0 == 0xc200u,
                "Original cache-flush exception vector did not execute and return through rfi.");
        for (unsigned index = 0; index < 128; ++index) {
            Require(ppc_read_u8(state, 0x81000000u + index) ==
                        ppc_read_u8(state, 0x802e0228u + index),
                    "Original audio startup did not install its real DSP initialization payload.");
        }
        Require(state.dspControl == 4 && state.dspHardware->aramConfiguration == 0x43u &&
                    state.dspHardware->resetCount == 1 && state.dspHardware->dmaTransfers == 2 &&
                    state.dspHardware->dmaMainAddress == 0x01000000u &&
                    state.dspHardware->dmaAramAddress == 0 && state.dspHardware->dmaCount == 32 &&
                    state.dspHardware->toDspMailbox == 0 && state.dspHardware->fromDspMailbox == 0,
                "Original audio reset/DMA chain fabricated DSP execution or lost actual transfer evidence.");
        for (unsigned index = 0; index < state.dspHardware->aram.size(); ++index) {
            Require(state.dspHardware->aram[index] ==
                        (index < 32 ? ppc_read_u8(state, 0x81000000u + index) : 0),
                    "Original audio ARAM DMA payload or full ARAM guard differs.");
        }
        auto noDspHardware = CreateOriginalStartupState(image);
        noDspHardware.dspHardware.reset();
        const auto dspFailure = RunOriginalStartup(dispatcher, noDspHardware);
        Require(dspFailure.pc == 0x801b2144u && dspFailure.reason.find("not mapped") != std::string::npos,
                "Missing DSP hardware profile was silently substituted.");
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
        Require(state.piInterruptMask == 0xf8u &&
                    ppc_read_u32(state, 0x80003040u + 20u * 4u) == 0x801f4d58u &&
                    state.serialInterface->polling == expectedPolling &&
                    state.serialInterface->communication == 0xe0010300u &&
                    state.serialInterface->status == 0x08000000u &&
                    ppc_read_u32(state, 0x802e5fe4u) == 0 &&
                    ppc_read_u32(state, 0x80340a60u) == UINT32_MAX,
                "Original SI sampling/interrupt setup differs or a controller completion was fabricated.");
        for (uint32_t channel = 1; channel < 4; ++channel) {
            Require(ppc_read_u32(state, 0x80340a60u + channel * 0x20u) == channel &&
                        ppc_read_u32(state, 0x80340a68u + channel * 0x20u) == 1 &&
                        ppc_read_u32(state, 0x80340a70u + channel * 0x20u) == 3 &&
                        ppc_read_u32(state, 0x80340a74u + channel * 0x20u) == 0x801f5b58u,
                    "Original SI queues did not retain the remaining controller probes.");
        }
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
        fixtures.Cleanup();
        std::cout << "Production original-startup binding, real entry/OSInit boundary, "
                     "dispatch/loop limits, disk/RAM integrity and missing-dependency checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "Original startup test failure: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
