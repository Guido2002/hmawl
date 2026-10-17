#include "original_startup.h"
#include "rel_image.h"
#include "generated_original_scene.h"
#include "native_console.h"
#include "native_exi_bus.h"
#include "native_si_bus.h"
#include "native_sram.h"

#include <chrono>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

// Keep large state temporaries out of the main Windows test stack.
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
std::unique_ptr<PpcState> StartupFixture(const DolImage& image) {
    return std::make_unique<PpcState>(CreateOriginalStartupState(image));
}

uint32_t VerifyOriginalSceneLink(const PpcState& state, const std::vector<uint8_t>& bytes,
                                 uint32_t module, uint32_t bss) {
    const auto image = RelImage::Parse(bytes);
    Require(image.id() == 2 && image.relocations().size() == 15170,
            "SystemTask did not load the expected original wlp1 REL.");
    const auto table = ppc_read_u32(state, module + 0x10);
    Require(table == module + 0x4c && ppc_read_u32(state, 0x800030c8u) == module &&
                ppc_read_u32(state, 0x800030ccu) == module,
            "Original OSLink did not publish the relocated module table/list.");
    std::vector<uint32_t> sections;
    for (size_t i = 0; i < image.sections().size(); ++i) {
        const auto& section = image.sections()[i];
        const uint32_t address = ppc_read_u32(state, table + static_cast<uint32_t>(i) * 8) & ~1u;
        Require(address == (section.size == 0 ? 0u :
                    section.fileOffset == 0 ? bss : module + section.fileOffset),
                "Original OSLink did not relocate a section to its actual allocation.");
        sections.push_back(address);
        if (section.size && section.fileOffset == 0) {
            const auto offset = ppc_memory_offset(state, address, section.size);
            Require(std::all_of(state.memory.begin() + offset,
                               state.memory.begin() + offset + section.size,
                               [](uint8_t value) { return value == 0; }),
                    "Original OSLink did not clear the allocated REL BSS.");
        }
    }
    for (const auto& fixup : image.relocations()) {
        if (fixup.importId != 0 && fixup.importId != image.id()) {
            const auto source = image.sections().at(fixup.patchSection).fileOffset + fixup.patchOffset;
            const auto patch = sections.at(fixup.patchSection) + fixup.patchOffset;
            const auto width = (fixup.type == 4 || fixup.type == 6) ? 2u : 4u;
            for (unsigned i = 0; i < width; ++i) {
                Require(ppc_read_u8(state, patch + i) == bytes.at(source + i),
                        "Original OSLink modified an import whose module is not loaded.");
            }
            continue;
        }
        const uint32_t target = fixup.addend +
            (fixup.importId ? sections.at(fixup.targetSection) : 0u);
        const uint32_t patch = sections.at(fixup.patchSection) + fixup.patchOffset;
        if (fixup.type == 1) {
            Require(ppc_read_u32(state, patch) == target, "Original OSLink ADDR32 mismatch.");
        } else if (fixup.type == 4 || fixup.type == 6) {
            const uint32_t expected = fixup.type == 4 ? target & 0xffffu :
                ((target >> 16) + ((target & 0x8000u) != 0 ? 1u : 0u)) & 0xffffu;
            Require(ppc_read_u16(state, patch) == expected, "Original OSLink LO/HA mismatch.");
        } else if (fixup.type == 10) {
            const auto word = ppc_read_u32(state, patch);
            const auto bits = word & 0x03fffffcu;
            const int64_t displacement = bits < 0x02000000u ? int64_t{bits} :
                int64_t{bits} - 0x04000000ll;
            const auto source = image.sections().at(fixup.patchSection).fileOffset + fixup.patchOffset;
            const uint32_t original = (uint32_t(bytes.at(source)) << 24) |
                (uint32_t(bytes.at(source + 1)) << 16) |
                (uint32_t(bytes.at(source + 2)) << 8) | bytes.at(source + 3);
            Require(static_cast<int64_t>(patch) + displacement == target &&
                        (word & 0xfc000003u) == (original & 0xfc000003u),
                    "Original OSLink REL24 target or instruction flags mismatch.");
        } else {
            throw std::runtime_error("Unexpected original scene relocation type.");
        }
    }
    const uint32_t prolog = sections.at(image.entrySections()[0]) + image.entryOffsets()[0];
    Require(ppc_read_u32(state, module + 0x34) == prolog,
            "Original OSLink did not relocate the module prolog.");
    return prolog;
}

template <typename Function>
void RequireFailure(Function function, const char* message) {
    bool failed = false;
    try { function(); } catch (const std::exception&) { failed = true; }
    Require(failed, message);
}

struct SceneConstructors {
    uint32_t module = 0;
    unsigned returned = 0;
    bool active = false;
    std::array<uint32_t, 32> registers{};
    std::array<PpcFpr, 32> floating{};

    void Observe(uint32_t pc, const PpcState& state) {
        if (!module) return;
        if (pc == module + 0x45b0cu) {
            Require(!active && returned < 28 &&
                        state.gpr[12] == ppc_read_u32(state, module + 0x46250u + returned * 4),
                    "Original scene constructor dispatch order differs.");
            std::copy(std::begin(state.gpr), std::end(state.gpr), registers.begin());
            floating = state.fpr;
            active = true;
        }
        if (pc == module + 0x45b14u) {
            Require(active && state.gpr[1] == registers[1] && state.gpr[2] == registers[2] &&
                        state.gpr[13] == registers[13],
                    "Original scene constructor changed stack/SDA state.");
            for (unsigned i = 14; i < 32; ++i) {
                Require(state.gpr[i] == registers[i] && state.fpr[i] == floating[i],
                        "Original scene constructor did not preserve nonvolatile registers.");
            }
            ++returned;
            active = false;
        }
    }
};

void TestScenePlacements(const DolImage& image, const std::vector<uint8_t>& bytes) {
    for (const auto& placement : std::array<std::array<uint32_t, 2>, 2>{{
             {{0x80508000u, 0x80e00000u}}, {{0x81012000u, 0x81504000u}}}}) {
        auto state = CreateOriginalStartupState(image);
        NativeDispatcher dispatcher;
        RegisterOriginalStartup(dispatcher, state);
        const auto module = placement[0], bss = placement[1];
        const auto offset = ppc_memory_offset(state, module, bytes.size());
        std::copy(bytes.begin(), bytes.end(), state.memory.begin() + offset);
        const auto bssOffset = ppc_memory_offset(state, bss, 0x4af4);
        std::fill_n(state.memory.begin() + bssOffset, 0x4af4, 0xa5);
        state.gpr[1] = 0x8035cfa0u;
        state.gpr[2] = 0x80351e40u;
        state.gpr[13] = 0x8034e9c0u;
        state.lr = 0x80003000u;
        dispatcher.Execute(state, 0x801b6988u);
        state.gpr[3] = module;
        state.gpr[4] = bss;
        state.lr = 0x80003000u;
        dispatcher.Execute(state, 0x801b4860u, 1000000);
        Require(state.gpr[3] == 1, "Original OSLink rejected an alternate scene placement.");
        const auto prolog = VerifyOriginalSceneLink(state, bytes, module, bss);
        const auto translations = OriginalSceneTranslations(state, module);
        const auto first = ppc_read_u32(state, prolog);
        ppc_write_u32(state, prolog, 0);
        RequireFailure([&] { dispatcher.Register(2, state, translations); },
                       "Corrupt scene code was bound as valid original code.");
        Require(!dispatcher.Contains(prolog), "Rejected scene registration was not atomic.");
        ppc_write_u32(state, prolog, first);
        dispatcher.Register(2, state, translations);
        SceneConstructors constructors;
        constructors.module = module;
        state.lr = 0x80003000u;
        bool reachedScene = false;
        try {
            dispatcher.Execute(state, prolog, 100000, 128, [&](uint32_t pc) {
                constructors.Observe(pc, state);
                if (pc == module + 0x45b8cu) {
                    reachedScene = true;
                    throw std::runtime_error("Test stops before using uninitialized game managers.");
                }
            });
        } catch (const std::exception&) {
            if (!reachedScene) throw;
        }
        Require(reachedScene && constructors.returned == 28 && !constructors.active &&
                    state.gpr[1] == 0x8035cf90u,
                "Relocatable prolog did not execute all original constructors at an alternate placement.");
        for (const auto& copy : std::array<std::array<uint32_t, 2>, 4>{{
                 {{0x4c158u, 0x8025116cu}}, {{0x4c164u, 0x80251178u}},
                 {{0x4c170u, 0x80251160u}}, {{0x4c17cu, 0x80251154u}}}}) {
            for (unsigned i = 0; i < 12; ++i) {
                Require(ppc_read_u8(state, module + copy[0] + i) == ppc_read_u8(state, copy[1] + i),
                        "Relocated original static constructor data differs from its DOL source.");
            }
        }
        const auto immediateWord = ppc_read_u32(state, prolog + 8);
        ppc_write_u32(state, prolog + 8, immediateWord ^ 1u);
        state.lr = 0x80003000u;
        RequireFailure([&] { dispatcher.Execute(state, prolog); },
                       "Modified relocated immediate bypassed native code integrity.");
        dispatcher.Unregister(2);
        Require(!dispatcher.Contains(prolog), "Scene code remained bound after unregister.");
    }
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
        auto unboundStorage = StartupFixture(image);
        auto& unboundState = *unboundStorage;
        NativeDispatcher unbound;
        const auto noBinding = RunOriginalStartup(unbound, unboundState);
        Require(noBinding.stop == OriginalStartupStop::Dependency &&
                    noBinding.pc == image.entryPoint() && !unboundState.callGuest &&
                    noBinding.reason.find("No native translation registered") != std::string::npos,
                "Unbound production dispatcher did not report its exact missing entry.");
        auto stateStorage = StartupFixture(image);
        auto& state = *stateStorage;
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
        auto peripheralStorage = StartupFixture(image);
        auto& missingPeripheral = *peripheralStorage;
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
                    state.dspHardware->dmaMainAddress == 0x01000020u &&
                    state.dspHardware->dmaAramAddress == 32 && state.dspHardware->dmaCount == 0 &&
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
        auto noDspHardwareStorage = StartupFixture(image);
        auto& noDspHardware = *noDspHardwareStorage;
        noDspHardware.dspHardware.reset();
        const auto dspFailure = RunOriginalStartup(dispatcher, noDspHardware);
        Require(dspFailure.pc == 0x801b2144u && dspFailure.reason.find("not mapped") != std::string::npos,
                "Missing DSP hardware profile was silently substituted.");
        auto noDspExecutionStorage = StartupFixture(image);
        auto& noDspExecution = *noDspExecutionStorage;
        noDspExecution.executeDspInitialization = {};
        const auto noDspResult = RunOriginalStartup(dispatcher, noDspExecution);
        Require(noDspResult.pc == 0x801b223cu &&
                    noDspResult.reason.find("DSP unhalt requires") != std::string::npos &&
                    noDspExecution.dspHardware->sentMail == 0 &&
                    noDspExecution.dspHardware->completedPrograms == 0,
                "Absent DSP compiler backend was silently replaced by a ready response.");
        auto damagedDspCodeStorage = StartupFixture(image);
        auto& damagedDspCode = *damagedDspCodeStorage;
        ppc_write_u16(damagedDspCode, 0x802e0228u, 0);
        const auto damagedDspResult = RunOriginalStartup(dispatcher, damagedDspCode);
        Require(damagedDspResult.pc == 0x801b223cu &&
                    damagedDspResult.reason.find("bootstrap payload differs") != std::string::npos &&
                    damagedDspCode.dspHardware->executedInstructions == 0 &&
                    damagedDspCode.dspHardware->sentMail == 0,
                "Modified DSP payload ran a stale compiled program or fabricated mail.");
        auto noMemoryProtectionStorage = StartupFixture(image);
        auto& noMemoryProtection = *noMemoryProtectionStorage;
        noMemoryProtection.memoryProtection.reset();
        const auto noMemoryResult = RunOriginalStartup(dispatcher, noMemoryProtection);
        Require(noMemoryResult.pc == 0x801b5094u && noMemoryResult.reason.find("not mapped") != std::string::npos,
                "Missing memory-protection profile was silently substituted.");
        auto noRamBatsStorage = StartupFixture(image);
        auto& noRamBats = *noRamBatsStorage;
        noRamBats.ramBats.reset();
        const auto noBatsResult = RunOriginalStartup(dispatcher, noRamBats);
        Require(noBatsResult.pc == 0x801b5068u && noBatsResult.reason.find("active system-call") != std::string::npos,
                "Missing RAM BAT profile silently skipped original real-mode configuration.");
        auto noColdResetStorage = StartupFixture(image);
        auto& noColdReset = *noColdResetStorage;
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
        auto missingDmaStorage = StartupFixture(image);
        auto& missingDma = *missingDmaStorage;
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
        auto noInterruptDeliveryStorage = StartupFixture(image);
        auto& noInterruptDelivery = *noInterruptDeliveryStorage;
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
            auto discStorage = StartupFixture(image);
            auto& discState = *discStorage;
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
            discState.readTimeBase = [&] {
                return ticks += (*discState.aiControl & 1u) ? 8 : 1000;
            };
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
            const auto& dsp = *discState.dspHardware;
            const auto& ai = *discState.audio;
            std::cout << "Original audio: AR/ARQ/AI flags=" << ppc_read_u32(discState, 0x80349844u)
                      << "/" << ppc_read_u32(discState, 0x8034986cu)
                      << "/" << ppc_read_u32(discState, 0x803497f8u)
                      << " ARAM internal/external=" << ppc_read_u32(discState, 0x80349830u)
                      << "/" << ppc_read_u32(discState, 0x80349834u)
                      << " DMA=" << dsp.dmaTransfers << " absent reads/writes=" << dsp.absentAramReads
                      << "/" << dsp.absentAramWrites << " AI samples=" << ai.advancedSamples
                      << " resets=" << ai.resets << " configuration=0x" << std::hex
                      << dsp.aramConfiguration << " PCM blocks=0x" << dsp.audioDmaBlocks
                      << " AI control=0x" << *discState.aiControl << " low-memory ARAM=0x"
                      << ppc_read_u32(discState, 0x800000d0u) << std::dec << '\n';
            const auto& task = *dsp.task;
            std::cout << "Original task: uploads=" << task.instructionUploads << " bytes=" << task.uploadedBytes
                      << " host mail=" << task.cpuMailSent << "/" << task.cpuMailConsumed
                      << " task IRQs=" << task.interrupts << " DSP mail=" << dsp.sentMail << "/" << dsp.consumedMail
                      << " DSP PC=" << dsp.instructionPc << " instructions=" << dsp.executedInstructions
                      << " task init flag=" << ppc_read_u32(discState, 0x80349a98u) << '\n';
            Require(ppc_read_u32(discState, 0x80349844u) == 1 &&
                        ppc_read_u32(discState, 0x8034986cu) == 1 &&
                        ppc_read_u32(discState, 0x803497f8u) == 1 &&
                        ppc_read_u32(discState, 0x80349830u) == 0x1000000 &&
                        ppc_read_u32(discState, 0x80349834u) == 0 &&
                        ppc_read_u32(discState, 0x800000d0u) == 0x1000000 &&
                        dsp.dmaTransfers == 5 && dsp.absentAramReads == 2 && dsp.absentAramWrites == 1 &&
                        dsp.aram.size() == 0x1000000 && dsp.aramConfiguration == 0x63 &&
                        dsp.completedPrograms == 1 && dsp.executedInstructions == 17507 &&
                        dsp.audioDmaBlocks == 0x14 && ai.volume == 0 && discState.aiControl == 0x46 &&
                        ai.advancedSamples == 4 && ai.resets == 5,
                    "Original AR/ARQ/AI startup flags, physical sizing, probe or sample-clock evidence differs.");
            Require(task.loaderStarts == 1 && task.instructionUploads == 1 && task.uploadedBytes == 6624 &&
                        task.cpuMailSent == 10 && task.cpuMailConsumed == 10 && task.interrupts == 1 &&
                        task.mode == NativeDspTaskMode::Running && task.source == 0x002e3da0 &&
                        task.destination == 0 && task.bytes == 6624 && task.entry == 16 &&
                        dsp.sentMail == 3 && dsp.consumedMail == 3 && dsp.instructionPc == 0x4a &&
                        !dsp.programHalted && dsp.dataRam[0x0e1b] == 0x0e80 && dsp.dataRam[0x0e31] == 0 &&
                        ppc_read_u32(discState, 0x80349a98u) == 1 &&
                        ppc_read_u32(discState, 0x80349870u) == 1 &&
                        ppc_read_u32(discState, 0x8033ea00u) == 1,
                    "Original DSP task upload, actual microcode stores/mail/IRQ and original callback evidence differs.");
            for (uint32_t word = 0; word < task.instructionRam.size(); ++word) {
                Require(task.instructionRam[word] ==
                            (word < task.bytes / 2 ? ppc_read_u16(discState, task.source + 2 * word) : 0),
                        "Native DSP IRAM upload changed original words or overran its DMA range.");
            }
            Require(discResult.stop == OriginalStartupStop::Dependency &&
                        discResult.pc == 0x801c165cu && discResult.lr == 0x801f2cacu &&
                        discResult.stack == 0x8035cec8u && discResult.osInitReturned &&
                        discResult.constructorsReturned == 93 && discResult.userInitReturned &&
                        discResult.gameMainEntered && discResult.memoryInitialized &&
                        discResult.screenInitEntered && discResult.gxInitEntered &&
                        discResult.gxInitReturned && discResult.viInitEntered &&
                        discResult.viInitReturned && discResult.viConfigured && discResult.viRetraceReturned &&
                        discResult.screenInitReturned && discResult.arInitReturned &&
                        discResult.arqInitReturned && discResult.aiInitReturned &&
                        discResult.dspInitReturned && discResult.dspTaskAdded && discResult.audioTaskInitReturned &&
                        !discResult.pcmDmaStartReturned && !discResult.gameInitializationReturned &&
                        !discState.callGuest && !discState.callException &&
                        discResult.reason == "Audio DMA enable requires a native PCM playback backend." &&
                        discState.discInterface->completedTransfers == 1 &&
                        discState.discInterface->status == 0x2au &&
                        discState.discInterface->dmaLength == 0 &&
                        discState.discInterface->control == 2 &&
                        discState.externalInterrupt->delivered == 170 &&
                        discState.externalInterrupt->returned == 170 && discState.threadContextReturns == 1 &&
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

            auto tracedStorage = StartupFixture(image);
            auto& traced = *tracedStorage;
            InstallOriginalStartupDisc(traced, disc);
            uint64_t tracedTicks = 1000000;
            traced.readTimeBase = [&] {
                return tracedTicks += (*traced.aiControl & 1u) ? 8 : 1000;
            };
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
            unsigned viHandlers = 0, dspHandlers = 0, dspInitCallbacks = 0;
            unsigned vectorReturns = 0, normalizationEntries = 0, normalizationReturns = 0;
            std::optional<std::pair<uint32_t, uint32_t>> normalizationReturn;
            std::optional<PpcState> peSource;
            uint64_t peSourceTicks = 0;
            std::vector<std::pair<uint32_t, uint32_t>> allocations;
            std::vector<std::pair<uint32_t, uint32_t>> allAllocations;
            std::optional<std::pair<uint32_t, uint32_t>> allocationRequest;
            std::array<uint32_t, 32> savedRegisters{};
            std::array<uint64_t, 18> savedFprs{};
            const auto observe = [&](uint32_t pc) {
                if (pc == 0x801c3f64u) ++dspHandlers;
                if (pc == 0x801f2b60u) ++dspInitCallbacks;
                if (pc == 0x8020ad8cu) {
                    ++vectorReturns;
                    Require(ppc_read_u32(traced, traced.gpr[1] + 0x2c) == 0 &&
                                ppc_read_u32(traced, traced.gpr[1] + 0x30) == 0xc1a00000u &&
                                ppc_read_u32(traced, traced.gpr[1] + 0x34) == 0xc3480000u,
                            "Original camera vector subtraction did not produce (0,-20,-200).");
                }
                if (pc == 0x801b8b1cu) {
                    ++normalizationEntries;
                    Require(!normalizationReturn, "Original vector normalization unexpectedly nested.");
                    normalizationReturn = std::make_pair(traced.lr, traced.gpr[4]);
                }
                if (normalizationReturn && pc == normalizationReturn->first) {
                    ++normalizationReturns;
                    const uint32_t normalized[][3] = {
                        {0, 0xbdcbc89c, 0xbf7ebac3}, {0, 0x3dcbc89a, 0x3f7ebac0},
                        {0x3f800000, 0x80000000, 0x80000000}
                    };
                    Require(normalizationReturns <= std::size(normalized),
                            "Original camera unexpectedly normalized another vector.");
                    if (normalizationReturns == 1) {
                        Require(traced.fpr[4] == PpcFpr{0x40e3ba0000000000, 0x3ff0000000000000},
                                "Original camera vector did not produce exact squared length 40400.");
                    }
                    std::cout << "Original normalized vector:";
                    for (unsigned component = 0; component < 3; ++component) {
                        const auto value = ppc_read_u32(traced, normalizationReturn->second + component * 4);
                        Require(value == normalized[normalizationReturns - 1][component],
                                "Original camera normalization did not store its exact unit-vector components.");
                        std::cout << " " << std::hex << value;
                    }
                    std::cout << std::dec << '\n';
                    normalizationReturn.reset();
                }
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
                reachedBoundary = traced.pc == 0x801c165cu &&
                    std::string(error.what()) == "Audio DMA enable requires a native PCM playback backend.";
                if (!reachedBoundary) throw;
            }
            std::cout << "Observed original initialization allocations=" << allocations.size()
                      << ", all returned allocations=" << allAllocations.size()
                      << ", allocator count=" << ppc_read_u32(traced, 0x80349494u) << '\n';
            Require(initializedHeap && allocations.size() == 11,
                    "Original initialization did not return all eleven observed heap/framebuffer allocations.");
            Require(!allocationRequest && allAllocations.size() == 178 &&
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
                        dspHandlers == 1 && dspInitCallbacks == 1 &&
                        vectorReturns == 1 && normalizationEntries == 3 && normalizationReturns == 3 &&
                        !normalizationReturn &&
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
            std::cout << "Observed VI handler returns=" << viHandlers
                      << " pending cause=0x" << std::hex << NativeViInterruptCause(vi) << std::dec << '\n';
            Require(vi.resets == 1 && vi.fields == 239 && viHandlers == 159 && vi.clockStarted &&
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
            const auto& observedAi = *traced.audio;
            const auto& observedDsp = *traced.dspHardware;
            const auto& observedTask = *observedDsp.task;
            Require(observedTask.mode == task.mode && observedTask.command == task.command &&
                        observedTask.parameters == task.parameters && observedTask.source == task.source &&
                        observedTask.destination == task.destination && observedTask.bytes == task.bytes &&
                        observedTask.dataBytes == task.dataBytes && observedTask.entry == task.entry &&
                        observedTask.instructionRam == task.instructionRam && observedTask.registers == task.registers &&
                        observedTask.loaderStarts == task.loaderStarts &&
                        observedTask.instructionUploads == task.instructionUploads &&
                        observedTask.uploadedBytes == task.uploadedBytes &&
                        observedTask.cpuMailSent == task.cpuMailSent &&
                        observedTask.cpuMailConsumed == task.cpuMailConsumed && observedTask.interrupts == task.interrupts,
                    "Observer changed original DSP task state, code, registers, loader or mail accounting.");
            Require(traced.aiControl == discState.aiControl && traced.dspControl == discState.dspControl &&
                        observedAi.volume == ai.volume && observedAi.samples == ai.samples &&
                        observedAi.interruptTiming == ai.interruptTiming && observedAi.lastTicks == ai.lastTicks &&
                        observedAi.phase == ai.phase && observedAi.advancedSamples == ai.advancedSamples &&
                        observedAi.writes == ai.writes && observedAi.resets == ai.resets &&
                        observedAi.clockStarted == ai.clockStarted &&
                        observedDsp.aram == dsp.aram && observedDsp.aramConfiguration == dsp.aramConfiguration &&
                        observedDsp.aramRefresh == dsp.aramRefresh &&
                        observedDsp.aramControllerReady == dsp.aramControllerReady &&
                        observedDsp.disconnectedAramExpansion == dsp.disconnectedAramExpansion &&
                        observedDsp.absentAramReads == dsp.absentAramReads &&
                        observedDsp.absentAramWrites == dsp.absentAramWrites &&
                        observedDsp.audioDmaAddress == dsp.audioDmaAddress &&
                        observedDsp.audioDmaBlocks == dsp.audioDmaBlocks &&
                        observedDsp.dmaMainAddress == dsp.dmaMainAddress &&
                        observedDsp.dmaAramAddress == dsp.dmaAramAddress && observedDsp.dmaCount == dsp.dmaCount &&
                        observedDsp.toDspMailbox == dsp.toDspMailbox && observedDsp.fromDspMailbox == dsp.fromDspMailbox &&
                        observedDsp.resetCount == dsp.resetCount && observedDsp.dmaTransfers == dsp.dmaTransfers &&
                        observedDsp.dataRam == dsp.dataRam && observedDsp.instructionPc == dsp.instructionPc &&
                        observedDsp.programHalted == dsp.programHalted &&
                        observedDsp.executedInstructions == dsp.executedInstructions &&
                        observedDsp.completedPrograms == dsp.completedPrograms &&
                        observedDsp.sentMail == dsp.sentMail && observedDsp.consumedMail == dsp.consumedMail,
                    "Observer changed AI timing/registers or DSP/ARAM DMA, mailbox and execution evidence.");
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
                auto peStorage = std::make_unique<PpcState>(interrupted);
                auto& pe = *peStorage;
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
            {
                auto interruptedStorage = std::make_unique<PpcState>(discState);
                auto& interrupted = *interruptedStorage;
                uint64_t aiTicks = ticks;
                interrupted.readTimeBase = [&] { return aiTicks += 8; };
                interrupted.piInterruptMask = 0x20;
                ppc_write_u32(interrupted, 0xcc006c0cu, 5);
                ppc_write_u32(interrupted, 0xcc006c00u, 0x2f);
                ppc_write_msr(interrupted, ppc_read_msr(interrupted) | 0x8000);
                aiTicks += 6744;
                NativeAiAdvance(interrupted, aiTicks);
                Require(NativeAiInterruptCause(interrupted) == 0x20,
                        "Test AI sample-clock threshold did not assert its interrupt.");
                const uint32_t resumePc = 0x801c3d48u;
                interrupted.pc = resumePc;
                auto resumedState = interrupted;
                unsigned handlers = 0, pendingViHandlers = 0;
                bool resumed = false;
                try {
                    dispatcher.Execute(interrupted, resumePc, 100000, 128, [&](uint32_t pc) {
                        if (pc == 0x801c1b44u) ++handlers;
                        if (pc == 0x801bde64u) ++pendingViHandlers;
                        if (pc == resumePc) throw std::runtime_error("AI test context resumed.");
                    });
                } catch (const std::exception& error) {
                    resumed = interrupted.pc == resumePc &&
                        std::string(error.what()) == "AI test context resumed.";
                    if (!resumed) throw;
                }
                std::cout << "AI interrupt evidence: resumed=" << resumed << " handlers=" << handlers
                          << " pending VI handlers=" << pendingViHandlers
                          << " pending=" << NativeAiInterruptCause(interrupted)
                          << " delivered/returned=" << interrupted.externalInterrupt->delivered
                          << "/" << interrupted.externalInterrupt->returned
                          << " expected=" << discState.externalInterrupt->returned + 1
                          << " active=" << interrupted.externalInterrupt->active
                          << " GPR=" << std::equal(std::begin(interrupted.gpr), std::end(interrupted.gpr),
                                                  std::begin(resumedState.gpr))
                          << " FPR=" << (interrupted.fpr == resumedState.fpr)
                          << " LR=" << (interrupted.lr == resumedState.lr)
                          << " CR=" << (interrupted.cr == resumedState.cr)
                          << " XER=" << (interrupted.xer == resumedState.xer)
                          << " CTR=" << (interrupted.ctr == resumedState.ctr)
                          << " MSR=" << (ppc_read_msr(interrupted) == ppc_read_msr(resumedState)) << '\n';
                Require(resumed && handlers == 1 && pendingViHandlers == 0 &&
                            NativeAiInterruptCause(interrupted) == 0 &&
                            NativeViInterruptCause(*interrupted.video) == 0 &&
                            interrupted.externalInterrupt->delivered == discState.externalInterrupt->delivered + 1 &&
                            interrupted.externalInterrupt->returned == discState.externalInterrupt->returned + 1 &&
                            !interrupted.externalInterrupt->active &&
                            std::equal(std::begin(interrupted.gpr), std::end(interrupted.gpr),
                                       std::begin(resumedState.gpr)) &&
                            interrupted.fpr == resumedState.fpr && interrupted.lr == resumedState.lr &&
                            interrupted.cr == resumedState.cr && interrupted.xer == resumedState.xer &&
                            interrupted.ctr == resumedState.ctr &&
                            ppc_read_msr(interrupted) == ppc_read_msr(resumedState),
                        "Original AI handler did not acknowledge the sample interrupt and restore context.");
                std::cout << "Test AI sample threshold executed the original AI handler and exception return; "
                             "this library fixture deliberately has no PCM backend.\n";
            }
            {
                auto pcmStorage = StartupFixture(image);
                auto& pcmState = *pcmStorage;
                InstallOriginalStartupDisc(pcmState, disc);
                pcmState.pcm.emplace();
                uint64_t pcmTicks = 1000000;
                uint64_t submittedFrames = 0;
                unsigned pcmHandlers = 0, pcmCallbacks = 0;
                unsigned commandHandshakes = 0;
                uint32_t commandAddress = 0;
                const std::array<uint32_t, 11> objectReturns = {
                    0x8000aa94u, 0x8000aaccu, 0x8000aaecu, 0x8000ab0cu,
                    0x8000ab2cu, 0x8000ab4cu, 0x8000ab6cu, 0x8000ab8cu,
                    0x8000abacu, 0x8000abccu, 0x8000ac04u};
                std::vector<uint32_t> returnedObjects;
                const std::array<uint32_t, 6> loopReturns = {
                    0x8000ac34u, 0x8000ac44u, 0x8000ac48u, 0x8000ac54u,
                    0x8000ac58u, 0x8000ac64u};
                unsigned loopSteps = 0, arrayConstructors = 0;
                unsigned loopEntries = 0;
                unsigned resourceReads = 0, fileCallbacks = 0, readNotifications = 0;
                uint64_t resourceBytes = 0;
                uint32_t pendingReadAddress = 0;
                std::vector<uint8_t> pendingRead;
                std::vector<uint8_t> sceneBytes;
                uint32_t sceneModule = 0, sceneBss = 0, sceneProlog = 0;
                uint32_t systemTask = 0;
                unsigned managerReturns = 0, loaderWorkers = 0, loaderExits = 0, alarmHandlers = 0;
                unsigned archiveHandles = 0;
                std::array<std::vector<uint32_t>, 3> taskMethods;
                std::array<std::vector<uint32_t>, 3> taskObjects;
                unsigned taskPass = 0;
                std::vector<uint32_t> deferredMethods, cleanupMethods;
                std::vector<uint32_t> updateQueue, sceneQueue, renderQueue;
                std::vector<uint32_t> renderMethods, renderCleanupMethods;
                std::vector<uint32_t> secondPassMethods;
                std::array<uint32_t, 7> cameraProjection{};
                bool perspectiveChecked = false, perspectiveApplied = false, projectionRestored = false;
                bool firstLoopQueuesCleared = false;
                uint32_t firstAssetState = 0, firstFadeActive = 0, firstManagerList = 0;
                uint8_t firstCameraActive = 0;
                const std::array<uint32_t, 7> frameReturns = {
                    0x8000acf8u, 0x8000ad14u, 0x8000ad2cu, 0x8000ad34u,
                    0x8000ad38u, 0x8000ad3cu, 0x8000ac34u};
                unsigned frameSteps = 0, padReads = 0;
                std::array<uint32_t, 3> frameBuffers{};
                uint32_t frameCounter = 0;
                uint32_t loopStack = 0;
                const auto readFloat = [&](uint32_t address) {
                    const auto bits = ppc_read_u32(pcmState, address);
                    float value;
                    std::memcpy(&value, &bits, sizeof(value));
                    return double(value);
                };
                const auto queuedObjects = [&](uint32_t list) {
                    std::vector<uint32_t> objects;
                    auto node = ppc_read_u32(pcmState, list + 4);
                    while (node != list + 0x14) {
                        Require(objects.size() < 8, "Original deferred task queue is cyclic or exceeds its bound.");
                        objects.push_back(ppc_read_u32(pcmState, node + 0xc));
                        node = ppc_read_u32(pcmState, node + 4);
                    }
                    return objects;
                };
                SceneConstructors sceneConstructors;
                const auto transfer = pcmState.transferDisc;
                pcmState.transferDisc = [&](const std::array<uint32_t, 3>& command, uint32_t length) {
                    auto bytes = transfer(command, length);
                    if (command[0] == 0xa8000000u) {
                        Require(pendingRead.empty(), "Original resource DMA was not observed before another read.");
                        Require(bytes == disc.Read(uint64_t(command[1]) * 4, length),
                                "Original resource read differs from actual disc bytes.");
                        pendingRead = bytes;
                        pendingReadAddress = pcmState.discInterface->dmaAddress;
                        ++resourceReads;
                        if (resourceReads == 2) sceneBytes = bytes;
                        resourceBytes += length;
                    }
                    return bytes;
                };
                const auto bindPcmTest = [](PpcState& guest, uint64_t& ticks, uint64_t& frames) {
                    guest.readTimeBase = [&guest, &ticks] {
                        return ticks += ((*guest.aiControl & 1u) || guest.pcm->active) ? 8 : 1000;
                    };
                    guest.writeConsole = [](const std::string&) {};
                    guest.submitPcm = [&frames](const std::vector<int16_t>& samples, uint32_t rate) {
                        Require(rate == 32028 && samples.size() == 320, "Original PCM buffer shape or DSP rate changed.");
                        Require(std::all_of(samples.begin(), samples.end(), [](int16_t sample) { return sample == 0; }),
                                "Original silent DSP output contains unexpected samples.");
                        frames += samples.size() / 2;
                    };
                    guest.stopPcm = [] { throw std::runtime_error("Unexpected PCM stop in original first-command fixture."); };
                };
                bindPcmTest(pcmState, pcmTicks, submittedFrames);
                const auto pcmResult = RunOriginalStartup(dispatcher, pcmState, 50000000, 128, [&](uint32_t pc) {
                    if (pc == 0x8000ac34u) {
                        ++loopEntries;
                        if (loopEntries == 1) loopStack = pcmState.gpr[1];
                        else Require(pcmState.gpr[1] == loopStack,
                                     "Original completed loop did not preserve main's stack pointer.");
                    }
                    if ((loopEntries == 1 && pc != 0x8000ac34u) ||
                        (loopEntries == 2 && pc == 0x8000ac34u)) {
                        if (std::find(frameReturns.begin(), frameReturns.end(), pc) != frameReturns.end()) {
                            Require(frameSteps < frameReturns.size() && pc == frameReturns[frameSteps],
                                    "Original frame tail and next-loop entry did not execute in order.");
                            ++frameSteps;
                        }
                    }
                    if (pc == 0x8000ad14u) {
                        Require(ppc_read_u32(pcmState, 0x80348fe0u) == 0,
                                "Original first frame unexpectedly installed a post-render callback.");
                    }
                    if (pc == 0x8000ad2cu) {
                        const auto screen = ppc_read_u32(pcmState, 0x803491b4u);
                        for (unsigned index = 0; index < frameBuffers.size(); ++index)
                            frameBuffers[index] = ppc_read_u32(pcmState, screen + index * 4);
                        Require(frameBuffers[0] && frameBuffers[1] && frameBuffers[0] != frameBuffers[1] &&
                                    frameBuffers[2] == frameBuffers[1] &&
                                    uint32_t(ppc_read_u32(pcmState, 0x8034975cu) - pcmState.gpr[27]) >=
                                        ppc_read_u32(pcmState, pcmState.gpr[29] + 0x54) - 1 &&
                                    pcmState.graphics->efb && pcmState.graphics->efb->lastCopy &&
                                    pcmState.graphics->efb->lastCopy->address == (frameBuffers[2] & 0x3fffffffu),
                                "Original frame did not pass its retrace gate with a copied alternate framebuffer.");
                        frameCounter = ppc_read_u32(pcmState, 0x803494a0u);
                    }
                    if (pc == 0x8000ad34u) {
                        const auto screen = ppc_read_u32(pcmState, 0x803491b4u);
                        Require(ppc_read_u32(pcmState, screen) == frameBuffers[0] &&
                                    ppc_read_u32(pcmState, screen + 4) == frameBuffers[1] &&
                                    ppc_read_u32(pcmState, screen + 8) == frameBuffers[0] &&
                                    ppc_read_u32(pcmState, 0x80318868u) == frameBuffers[2] &&
                                    ppc_read_u8(pcmState, 0x803494b4u) == 1 &&
                                    ppc_read_u32(pcmState, 0x803494b0u) == 1,
                                "Original Screen did not request the copied VI framebuffer and swap its draw buffer.");
                    }
                    if (pc == 0x8000ad38u)
                        Require(ppc_read_u32(pcmState, 0x803494a0u) == frameCounter + 1,
                                "Original timing update did not increment its frame counter.");
                    if (pc == 0x80212744u) {
                        Require(pcmState.gpr[3] == 0, "Original disconnected PADRead reported motor-capable controllers.");
                        for (unsigned channel = 0; channel < 4; ++channel) {
                            const auto status = pcmState.gpr[1] + 8 + channel * 12;
                            Require(ppc_read_u8(pcmState, status + 10) == 0xff,
                                    "Original PADRead did not report a disconnected controller.");
                            for (unsigned byte = 0; byte < 10; ++byte)
                                Require(ppc_read_u8(pcmState, status + byte) == 0,
                                        "Original disconnected PADRead published nonzero buttons or axes.");
                        }
                        ++padReads;
                    }
                    if (pc == 0x8000ad3cu) {
                        Require(padReads == 1 && ppc_read_u8(pcmState, 0x80341433u) == 0,
                                "Original controller update did not consume its queued PAD sample.");
                        for (unsigned channel = 0; channel < 4; ++channel) {
                            const auto status = 0x80341574u + channel * 0x44;
                            Require(ppc_read_u8(pcmState, status + 0x42) == 0xff &&
                                        ppc_read_u32(pcmState, status) == 0 &&
                                        ppc_read_u32(pcmState, status + 8) == 0 &&
                                        ppc_read_u32(pcmState, status + 0x10) == 0,
                                    "Original controller publication fabricated buttons or connected status.");
                        }
                        const auto tasks = ppc_read_u32(pcmState, 0x8034923cu);
                        Require(queuedObjects(tasks + 0x74).empty() && queuedObjects(tasks + 0x48).empty(),
                                "Original first loop did not clear both deferred queues.");
                        for (const auto* queue : {&updateQueue, &renderQueue})
                            for (const auto node : *queue)
                                Require(ppc_read_u32(pcmState, node) == 0 && ppc_read_u32(pcmState, node + 4) == 0,
                                        "Original first-loop cleanup did not unlink every scheduled node.");
                        firstLoopQueuesCleared = true;
                        firstAssetState = ppc_read_u32(pcmState, taskObjects[0].back() + 0x1a4);
                        firstFadeActive = ppc_read_u32(pcmState, taskObjects[1].back() + 0x60);
                        firstManagerList = ppc_read_u32(pcmState, returnedObjects.back() + 0x4c);
                        firstCameraActive = ppc_read_u8(pcmState, ppc_read_u32(pcmState, 0x80349238u) + 4);
                    }
                    if (pc == 0x8017d018u) {
                        const auto tasks = ppc_read_u32(pcmState, 0x8034923cu);
                        updateQueue = queuedObjects(tasks + 0x74);
                        sceneQueue = queuedObjects(tasks + 0x48);
                    }
                    if (pc == 0x8017cf70u) {
                        renderQueue = queuedObjects(ppc_read_u32(pcmState, 0x8034923cu) + 0x48);
                        const auto gx = ppc_read_u32(pcmState, pcmState.gpr[2] - 0x5f48u);
                        for (unsigned index = 0; index < cameraProjection.size(); ++index)
                            cameraProjection[index] = ppc_read_u32(pcmState, gx + 0x4d8 + index * 4);
                    }
                    if (pc == 0x80181ec4u) {
                        const auto angle = float(float(readFloat(0x8034bec0u)) * float(readFloat(0x8034bb60u)));
                        const auto radians = float(float(readFloat(0x8034bec4u)) * angle);
                        const auto cotangent = 1.0 / std::tan(double(radians));
                        const auto nearPlane = readFloat(0x8034bb68u), farPlane = readFloat(0x8034bb6cu);
                        const std::array<double, 16> expected = {
                            cotangent / readFloat(0x8034bb64u), 0, 0, 0,
                            0, cotangent, 0, 0, 0, 0, -nearPlane / (farPlane - nearPlane),
                            -farPlane * nearPlane / (farPlane - nearPlane), 0, 0, -1, 0};
                        for (unsigned index = 0; index < expected.size(); ++index)
                            Require(std::abs(readFloat(pcmState.gpr[1] + 8 + index * 4) - expected[index]) <
                                        0.00001 * std::max(1.0, std::abs(expected[index])),
                                    "Original tangent/perspective chain produced an incorrect projection matrix.");
                        perspectiveChecked = true;
                    }
                    if (pc == 0x80181ed0u) {
                        const auto gx = ppc_read_u32(pcmState, pcmState.gpr[2] - 0x5f48u);
                        Require(ppc_read_u32(pcmState, gx + 0x4d8) == 0,
                                "Original kernel did not select the perspective GX projection.");
                        const std::array<unsigned, 6> offsets = {0, 8, 20, 24, 40, 44};
                        for (unsigned index = 0; index < offsets.size(); ++index)
                            Require(ppc_read_u32(pcmState, gx + 0x4dc + index * 4) ==
                                        ppc_read_u32(pcmState, pcmState.gpr[1] + 8 + offsets[index]),
                                    "Original kernel did not apply its computed perspective coefficients to GX.");
                        perspectiveApplied = true;
                    }
                    if (pc == 0x80181f14u) {
                        const auto gx = ppc_read_u32(pcmState, pcmState.gpr[2] - 0x5f48u);
                        Require(readFloat(0x80315600u) == double(cameraProjection[0]),
                                "Original kernel did not save the projection type.");
                        for (unsigned index = 0; index < cameraProjection.size(); ++index) {
                            Require(ppc_read_u32(pcmState, gx + 0x4d8 + index * 4) == cameraProjection[index],
                                    "Original kernel did not restore the saved camera projection.");
                            if (index)
                                Require(ppc_read_u32(pcmState, 0x80315600u + index * 4) == cameraProjection[index],
                                        "Original kernel saved incorrect projection coefficients.");
                        }
                        projectionRestored = true;
                    }
                    if (pc == 0x80235d7cu &&
                        (pcmState.lr == 0x8017d180u || pcmState.lr == 0x8017d1e0u)) {
                        const auto descriptor = pcmState.gpr[12];
                        const auto vtable = ppc_read_u32(pcmState, pcmState.gpr[3] +
                            ppc_read_u32(pcmState, descriptor) + ppc_read_u32(pcmState, descriptor + 8));
                        auto& methods = pcmState.lr == 0x8017d180u ? renderMethods : renderCleanupMethods;
                        Require(methods.size() < 8, "Original render callback or cleanup repeated unexpectedly.");
                        methods.push_back(ppc_read_u32(pcmState, vtable + ppc_read_u32(pcmState, descriptor + 4)));
                    }
                    if (pc == 0x80235d7cu &&
                        (pcmState.lr == 0x8017d29cu || pcmState.lr == 0x8017d2fcu)) {
                        const auto descriptor = pcmState.gpr[12];
                        const auto vtable = ppc_read_u32(pcmState, pcmState.gpr[3] +
                            ppc_read_u32(pcmState, descriptor) + ppc_read_u32(pcmState, descriptor + 8));
                        auto& methods = pcmState.lr == 0x8017d29cu ? deferredMethods : cleanupMethods;
                        Require(methods.size() < 2, "Original deferred update or cleanup repeated unexpectedly.");
                        methods.push_back(ppc_read_u32(pcmState, vtable + ppc_read_u32(pcmState, descriptor + 4)));
                    }
                    if (pc == 0x8000ac80u || pc == 0x8000ac8cu || pc == 0x8000ac98u) {
                        Require(taskPass < 3 && pc == 0x8000ac80u + taskPass * 12,
                                "Original main did not complete its three task passes in order.");
                        ++taskPass;
                    }
                    if (pc == 0x80235d7cu && pcmState.lr == 0x8017d0c0u && loopEntries == 1) {
                        Require(taskPass < taskMethods.size(), "Unexpected additional original task pass.");
                        const auto descriptor = pcmState.gpr[12];
                        const std::array<uint32_t, 3> slots = {0x10, 0x18, 0x14};
                        Require(ppc_read_u32(pcmState, descriptor) == 0 &&
                                    ppc_read_u32(pcmState, descriptor + 4) == slots[taskPass] &&
                                    ppc_read_u32(pcmState, descriptor + 8) == 0x10 &&
                                    pcmState.gpr[4] == 0x800 && taskMethods[taskPass].size() < 8,
                                "Original task member-function descriptor or update mask changed.");
                        const auto vtable = ppc_read_u32(pcmState, pcmState.gpr[3] + 0x10);
                        taskMethods[taskPass].push_back(ppc_read_u32(pcmState, vtable + slots[taskPass]));
                        taskObjects[taskPass].push_back(pcmState.gpr[3]);
                    }
                    if (pc == 0x80235d7cu && pcmState.lr == 0x8017d0c0u && loopEntries == 2) {
                        Require(secondPassMethods.size() < 8, "Original second-loop task pass exceeded its bound.");
                        const auto vtable = ppc_read_u32(pcmState, pcmState.gpr[3] + 0x10);
                        secondPassMethods.push_back(ppc_read_u32(pcmState, vtable + 0x10));
                    }
                    if (pc == 0x8015468cu) ++managerReturns;
                    if (pc == 0x80199dd4u) ++loaderWorkers;
                    if (pc == 0x801b72bcu) ++loaderExits;
                    if (pc == 0x801b1c84u) ++alarmHandlers;
                    if (pc == 0x8019a0b8u) {
                        const auto archive = pcmState.gpr[31];
                        const auto handle = pcmState.gpr[30];
                        Require(pcmState.gpr[3] == 1 && ppc_read_u32(pcmState, archive) == 0x55aa382du &&
                                    ppc_read_u32(pcmState, handle) == archive &&
                                    ppc_read_u32(pcmState, handle + 4) ==
                                        archive + ppc_read_u32(pcmState, archive + 4),
                                "Original ARCInitHandle did not publish the loaded archive and its file table.");
                        ++archiveHandles;
                    }
                    if (pc == 0x801b4860u) {
                        sceneModule = pcmState.gpr[3];
                        sceneBss = pcmState.gpr[4];
                        Require(sceneModule && sceneBss && sceneBytes.size() == 433312,
                                "Original scene link did not receive its loaded image and BSS.");
                    }
                    if (pc == 0x80177c6cu) {
                        Require(pcmState.gpr[3] == 1, "Original OSLink rejected the scene module.");
                        systemTask = pcmState.gpr[31];
                        sceneProlog = VerifyOriginalSceneLink(pcmState, sceneBytes, sceneModule, sceneBss);
                        sceneConstructors.module = sceneModule;
                    }
                    sceneConstructors.Observe(pc, pcmState);
                    if (!pendingRead.empty()) {
                        const auto offset = ppc_memory_offset(pcmState, pendingReadAddress, pendingRead.size());
                        Require(std::equal(pendingRead.begin(), pendingRead.end(), pcmState.memory.begin() + offset),
                                "Original resource DMA did not write exact disc bytes into guest RAM.");
                        pendingRead.clear();
                    }
                    if (std::find(objectReturns.begin(), objectReturns.end(), pc) != objectReturns.end()) {
                        Require(returnedObjects.size() < objectReturns.size() &&
                                    pc == objectReturns[returnedObjects.size()] && pcmState.gpr[3] != 0,
                                "Original startup objects did not return in main's original order.");
                        returnedObjects.push_back(pcmState.gpr[3]);
                        if (pc == objectReturns.back()) {
                            const auto manager = pcmState.gpr[3];
                            Require(ppc_read_u32(pcmState, manager + 0x4c) == 0 &&
                                        ppc_read_u32(pcmState, manager + 0xa8) == 0 &&
                                        ppc_read_u32(pcmState, manager + 0xac) == 0,
                                    "Original manager constructor did not reset both task lists.");
                        }
                    }
                    if (pc == 0x8017acf0u) ++fileCallbacks;
                    if (pc == 0x80176754u && !sceneModule) ++arrayConstructors;
                    if (loopEntries <= 1 &&
                        std::find(loopReturns.begin(), loopReturns.end(), pc) != loopReturns.end()) {
                        Require(loopSteps < loopReturns.size() && pc == loopReturns[loopSteps],
                                "Original first-loop preparation did not execute in main's call order.");
                        ++loopSteps;
                    }
                    if (pc == 0x8017b558u) ++readNotifications;
                    if (pc == 0x8017ad4cu) {
                        Require(pcmState.gpr[3] == 0 && ppc_read_u8(pcmState, pcmState.gpr[31] + 0x55) == 1,
                                "Original DVD notification did not set the file's completed-read flag.");
                    }
                    if (pc == 0x801c1bc0u) ++pcmHandlers;
                    if (pc == 0x801f2ab8u) ++pcmCallbacks;
                    if (pc == 0x801f2e64u) commandAddress = pcmState.gpr[31];
                    if (pc == 0x801f2e70u) ++commandHandshakes;
                });
                std::cout << "PCM integration: PC=" << std::hex << pcmResult.pc << " LR=" << pcmResult.lr << std::dec
                          << " reason=" << pcmResult.reason << " frames=" << submittedFrames
                          << " periods=" << pcmState.pcm->completedBuffers << " handlers=" << pcmHandlers
                          << " callbacks=" << pcmCallbacks << " DSP PC=" << pcmState.dspHardware->instructionPc
                          << " DMA=" << pcmState.dspHardware->task->dataTransfers << "/"
                          << pcmState.dspHardware->task->transferredBytes << " command returns=" << commandHandshakes
                          << " CPU mail=" << pcmState.dspHardware->task->cpuMailSent << "/"
                          << pcmState.dspHardware->task->cpuMailConsumed
                          << " DSP mail=" << pcmState.dspHardware->sentMail << "/" << pcmState.dspHardware->consumedMail
                          << " task IRQ=" << pcmState.dspHardware->task->interrupts
                          << " active IRQ=" << pcmState.externalInterrupt->active
                          << " suspended IRQ=" << pcmState.externalInterrupt->suspended.size()
                          << " SRR=" << std::hex << pcmState.srr0 << "/" << pcmState.srr1
                          << " resume=" << pcmState.externalInterrupt->resume << std::dec
                          << " objects=" << returnedObjects.size()
                          << " reads=" << resourceReads << "/" << resourceBytes
                          << " file callbacks=" << fileCallbacks << "/" << readNotifications << '\n';
                std::cout << "Scene manager returns=" << managerReturns
                          << " loader workers/exits=" << loaderWorkers << "/" << loaderExits
                          << " alarm handlers/delivered/returned=" << alarmHandlers << "/"
                          << pcmState.decrementer->delivered << "/" << pcmState.decrementer->returned
                          << " thread context returns=" << pcmState.threadContextReturns
                          << " archive handles=" << archiveHandles << '\n';
                Require(pcmResult.stop == OriginalStartupStop::Dependency &&
                            sceneProlog != 0 && pcmResult.pc == 0x800e10c4u &&
                            pcmResult.lr == sceneModule + 0x3ec80u &&
                            pcmResult.sceneModuleLinked && pcmResult.scenePrologEntered &&
                            pcmResult.sceneInitializationEntered && pcmResult.scenePrologReturned &&
                            pcmResult.sceneConstructorsReturned == 28 && pcmResult.systemTaskReturned &&
                            pcmResult.taskPassesReturned == 3 &&
                            pcmResult.deferredUpdatesReturned && pcmResult.deferredUpdateQueueCleared &&
                            pcmResult.screenPreparationReturned && pcmResult.renderQueuePrepared &&
                            pcmResult.renderPassReturned && pcmResult.renderQueueCleared &&
                            pcmResult.frameDrawSyncReturned &&
                            pcmResult.postRenderCallbackReturned && pcmResult.frameRetraceWaitReturned &&
                            pcmResult.frameBufferExchangeReturned && pcmResult.frameTimingReturned &&
                            pcmResult.controllerUpdateReturned &&
                            pcmResult.gameLoopIterationsCompleted == 1 && loopEntries == 2 &&
                            frameSteps == frameReturns.size() && padReads == 1 &&
                            sceneConstructors.returned == 28 && !sceneConstructors.active &&
                            !dispatcher.Contains(sceneProlog) &&
                            pcmResult.audioTaskInitReturned && pcmResult.pcmDmaStartReturned &&
                            pcmResult.gameInitializationReturned && pcmResult.gameLoopEntered &&
                            pcmResult.firstLoopPreparationReturned &&
                            pcmState.pcm->active && pcmState.pcm->starts == 1 &&
                            pcmState.pcm->completedBuffers == 4210 && pcmState.pcm->submittedFrames == submittedFrames &&
                            submittedFrames == 673760 && pcmHandlers == 4207 && pcmCallbacks == 4204 &&
                            commandAddress != 0 && commandHandshakes == 4204 &&
                            pcmState.dspHardware->task->cpuMailSent == 12622 &&
                            pcmState.dspHardware->task->cpuMailConsumed == 12622 &&
                            !(pcmState.dspHardware->toDspMailbox & 0x80000000u) &&
                            pcmState.dspHardware->task->dataTransfers == 134495 &&
                            pcmState.dspHardware->task->transferredBytes == 40817692 &&
                            pcmState.dspHardware->instructionPc == 0x4a &&
                            pcmState.dspHardware->task->interrupts == 4205 &&
                            pcmState.dspHardware->sentMail == 4207 && pcmState.dspHardware->consumedMail == 4207 &&
                            pcmState.dspHardware->task->mode == NativeDspTaskMode::Running &&
                            pcmState.dspHardware->completedPrograms == 1 &&
                            !pcmState.systemCallActive && !pcmState.externalInterrupt->active &&
                            pcmState.externalInterrupt->suspended.empty(),
                        "Original scene prolog did not finish through the real loader-thread and interrupt chain.");
                Require(returnedObjects.size() == objectReturns.size() &&
                            loopSteps == loopReturns.size() && arrayConstructors == 2 &&
                            pcmResult.startupObjectsReturned == objectReturns.size() &&
                            pcmResult.resourceReadNotifications == fileCallbacks &&
                            resourceReads == 31 && resourceBytes == 8819232 &&
                            fileCallbacks == 21 && readNotifications == 4 && pendingRead.empty(),
                        "Original startup resource reads did not complete through the real DVD callback chain.");
                const auto scene = ppc_read_u32(pcmState, systemTask + 0x4c);
                const auto loader = ppc_read_u32(pcmState, 0x80349174u);
                Require(managerReturns == 1 && loaderWorkers == 52 && loaderExits == loaderWorkers &&
                            alarmHandlers == 1 && pcmState.decrementer->delivered == alarmHandlers &&
                            pcmState.decrementer->returned == alarmHandlers &&
                            pcmState.threadContextReturns == 54 && archiveHandles > 0 &&
                            scene != 0 && ppc_read_u32(pcmState, scene + 0x14) == sceneModule + 0x47da4u &&
                            ppc_read_u32(pcmState, scene + 0x18) != 0 &&
                            ppc_read_u32(pcmState, scene + 0x34) == 24 &&
                            ppc_read_u32(pcmState, loader + 0xcc) == ppc_read_u32(pcmState, loader + 0xd0),
                        "Original scene was not published with state-24 assets and a fully drained loader queue.");
                const std::array<std::vector<uint32_t>, 3> expectedMethods = {{
                    {0x80177648u, 0x8012a3c8u, 0x8017649cu, 0x80197f58u,
                     0x80154bd0u, 0x8008616cu, sceneModule + 0x3e734u},
                    {0x801782e4u, 0x80009540u, 0x80009540u, 0x80009540u,
                     0x80009540u, 0x80086634u, sceneModule + 0x42f4u, 0x801894ecu},
                    {0x801782ecu, 0x80009538u, 0x80009538u, 0x80009538u,
                     0x80009538u, 0x8008663cu, sceneModule + 0x42ecu, 0x801894f4u}
                }};
                Require(taskPass == 3 && taskMethods == expectedMethods &&
                            taskObjects[0].front() == systemTask &&
                            taskObjects[0].back() == ppc_read_u32(pcmState, scene + 0x18) &&
                            std::equal(taskObjects[0].begin(), taskObjects[0].end(), taskObjects[1].begin()) &&
                            taskObjects[1] == taskObjects[2],
                        "Original task passes did not call every registered virtual method in priority order.");
                const auto fade = taskObjects[1].back();
                Require(firstAssetState == 1 && firstFadeActive == 1 &&
                            ppc_read_u32(pcmState, fade + 0x14) == 0x10000,
                        "Original asset update did not enter its first fade state and register the fade task.");
                const auto tasks = ppc_read_u32(pcmState, 0x8034923cu);
                Require(updateQueue ==
                            std::vector<uint32_t>{taskObjects[0][1] + 0x30, taskObjects[0][5] + 0x30} &&
                            sceneQueue ==
                            std::vector<uint32_t>{taskObjects[0][5] + 0x18, taskObjects[0][4] + 0x18},
                        "Original updates did not build the ordered audio/camera and camera/scene deferred queues.");
                Require(deferredMethods == std::vector<uint32_t>{0x8012ab20u, 0x8008661cu} &&
                            cleanupMethods == std::vector<uint32_t>{0x800094c8u, 0x800094c8u} &&
                            firstLoopQueuesCleared,
                        "Original deferred audio/camera callbacks did not return and unlink their queue nodes.");
                Require(renderQueue == std::vector<uint32_t>{
                            0x80315534u, 0x80315558u, 0x8031557cu, 0x803155a0u,
                            0x803155c4u, sceneQueue[0], 0x803155e8u, sceneQueue[1]},
                        "Original kernel did not merge its six render tasks with camera/scene in sorted order.");
                Require(renderMethods == std::vector<uint32_t>{
                            0x80181e7cu, 0x80181efcu, 0x80181f6cu, 0x80181f94u,
                            0x80181fd4u, 0x8008660cu, 0x80182048u, 0x80154fb4u} &&
                            renderCleanupMethods == std::vector<uint32_t>(8, 0x800094e8u) &&
                            perspectiveChecked && perspectiveApplied && projectionRestored &&
                            firstLoopQueuesCleared,
                        "Original sorted render pass, perspective restoration or queue cleanup did not complete.");
                Require(secondPassMethods == expectedMethods[0] &&
                            queuedObjects(tasks + 0x74) == updateQueue &&
                            queuedObjects(tasks + 0x48) == sceneQueue,
                        "Original second loop did not resume its first task pass and rebuild the deferred queues.");
                const auto listener = ppc_read_u32(pcmState, taskObjects[0][1] + 0x54) + 0x84;
                const std::array<uint32_t, 12> listenerInverse = {
                    0xbf800000u, 0x80000000u, 0, 0x80000000u, 0x3f800000u, 0x80000000u,
                    0, 0, 0xbf800000u, 0, 0, 0};
                for (unsigned index = 0; index < listenerInverse.size(); ++index) {
                    Require(ppc_read_u32(pcmState, listener + 0x50 + index * 4) == listenerInverse[index],
                            "Original audio listener did not publish its inverse orientation/translation.");
                }
                Require(ppc_read_u32(pcmState, listener + 0x8c) == 0x3f800000u,
                        "Original listener did not normalize its original 127-volume input to one.");
                TestScenePlacements(image, sceneBytes);
                const auto camera = returnedObjects.front();
                const auto view = ppc_read_u32(pcmState, 0x80349238u);
                Require(ppc_read_u32(pcmState, view + 8) == camera + 0x9c &&
                            firstCameraActive == 1 && ppc_read_u8(pcmState, view + 4) == 0,
                        "Original camera did not publish its view and reset its active flag for the second loop.");
                for (unsigned row = 0; row < 3; ++row) {
                    double origin = readFloat(camera + 0x9c + row * 16 + 12);
                    double direction = 0;
                    for (unsigned column = 0; column < 3; ++column) {
                        const auto coefficient = readFloat(camera + 0x9c + row * 16 + column * 4);
                        origin += coefficient * readFloat(view + 0xc + column * 4);
                        direction += coefficient * readFloat(view + 0x18 + column * 4);
                    }
                    Require(std::abs(origin) < 0.00001 &&
                                std::abs(direction - readFloat(0x8026aa40u + row * 4)) < 0.00001,
                            "Original inverse camera origin/direction does not map back through its source matrix.");
                }
                const auto gxData = ppc_read_u32(pcmState, pcmState.gpr[2] - 0x5f48u);
                const auto nearPlane = readFloat(0x8034a644u);
                const auto farPlane = readFloat(0x8034a66cu);
                const std::array<double, 6> projection = {
                    1.0 / 320, 0, -1.0 / 224, 0,
                    -1.0 / (farPlane - nearPlane), -farPlane / (farPlane - nearPlane)};
                const std::array<unsigned, 6> projectionOffsets = {0, 12, 20, 28, 40, 44};
                Require(ppc_read_u32(pcmState, gxData + 0x4d8) == 1,
                        "Original camera did not select the orthographic GX projection.");
                for (unsigned index = 0; index < projection.size(); ++index) {
                    Require(std::abs(readFloat(camera + 0x5c + projectionOffsets[index]) - projection[index]) < 0.000001 &&
                                ppc_read_u32(pcmState, gxData + 0x4dc + index * 4) ==
                                    ppc_read_u32(pcmState, camera + 0x5c + projectionOffsets[index]),
                            "Original camera projection differs from its bounds or original GX projection state.");
                }
                Require(ppc_read_u32(pcmState, 0x80349228u) == camera &&
                            ppc_read_u32(pcmState, camera + 0x10) == 0x802aba5cu &&
                            ppc_read_u32(pcmState, camera + 0x28) == 0x802aba78u &&
                            ppc_read_u32(pcmState, camera + 0x40) == 0x802aba8cu &&
                            ppc_read_u32(pcmState, camera + 0x48) == 0x802abaa0u &&
                            ppc_read_u32(pcmState, camera + 0x4c) == 0xc3600000u &&
                            ppc_read_u32(pcmState, camera + 0x50) == 0x43600000u &&
                            ppc_read_u32(pcmState, camera + 0x54) == 0xc3a00000u &&
                            ppc_read_u32(pcmState, camera + 0x58) == 0x43a00000u,
                        "Original camera construction did not preserve its vtables and 640x448 projection bounds.");
                const auto object = returnedObjects[8];
                Require(ppc_read_u32(pcmState, 0x803491d4u) == object &&
                            ppc_read_u32(pcmState, object) == 0x80298670u &&
                            ppc_read_u32(pcmState, object + 0x2c) == 19 &&
                            ppc_read_u32(pcmState, object + 0x30) == 19,
                        "Original final startup object lost its singleton, vtable or initial state.");
                for (unsigned offset : {4u, 12u, 20u}) {
                    Require(ppc_read_u32(pcmState, object + offset) == UINT32_MAX &&
                                ppc_read_u32(pcmState, object + offset + 4) == 0x80297e90u,
                            "Original startup object did not initialize all three handles.");
                }
                const auto manager = returnedObjects.back();
                Require(ppc_read_u32(pcmState, 0x803491e0u) == manager &&
                            firstManagerList == 1 && ppc_read_u32(pcmState, manager + 0x4c) == 0 &&
                            ppc_read_u32(pcmState, manager + 0xa8) == 0 &&
                            ppc_read_u32(pcmState, manager + 0xac) == 0,
                        "Original manager did not toggle its active task list during its first update.");
                for (unsigned offset : {0x50u, 0x7cu}) {
                    Require(ppc_read_u32(pcmState, manager + offset + 0x10) == 0x802c9a14u &&
                                ppc_read_u32(pcmState, manager + offset + 0x24) == 0x802c9a14u &&
                                ppc_read_u32(pcmState, manager + offset + 0x28) == 0x802c9a20u,
                            "Original manager array constructor lost its element stride or vtables.");
                }
                for (unsigned i = 0; i < 18; ++i) {
                    Require(pcmState.dspHardware->dataRam[0xc00 + i] == ppc_read_u16(pcmState, commandAddress + i * 2),
                            "DSP command-list DMA differs from original CPU RAM.");
                }
                auto plainStorage = StartupFixture(image);
                auto& plainPcm = *plainStorage;
                InstallOriginalStartupDisc(plainPcm, disc);
                plainPcm.pcm.emplace();
                uint64_t plainTicks = 1000000, plainFrames = 0;
                bindPcmTest(plainPcm, plainTicks, plainFrames);
                const auto plainResult = RunOriginalStartup(dispatcher, plainPcm, 50000000);
                Require(DescribeOriginalStartupStop(pcmResult).find("Verified complete original game-loop iterations: 1.") !=
                            std::string::npos &&
                            DescribeOriginalStartupStop(pcmResult).find("no complete game-loop iteration") == std::string::npos &&
                            DescribeOriginalStartupStop(pcmResult).find("no rendered game frame is verified.") != std::string::npos,
                        "Original startup report omitted the verified loop or claimed a rendered game frame.");
                Require(plainResult.pc == pcmResult.pc && plainResult.lr == pcmResult.lr &&
                            plainResult.reason == pcmResult.reason && plainTicks == pcmTicks &&
                            plainFrames == submittedFrames && plainPcm.memory == pcmState.memory &&
                            std::equal(std::begin(plainPcm.gpr), std::end(plainPcm.gpr), std::begin(pcmState.gpr)) &&
                            plainPcm.fpr == pcmState.fpr && plainPcm.fpscr == pcmState.fpscr &&
                            plainPcm.gqr == pcmState.gqr &&
                            plainPcm.cr == pcmState.cr && plainPcm.xer == pcmState.xer &&
                            plainPcm.ctr == pcmState.ctr && plainPcm.lr == pcmState.lr &&
                            plainPcm.srr0 == pcmState.srr0 && plainPcm.srr1 == pcmState.srr1 &&
                            ppc_read_msr(plainPcm) == ppc_read_msr(pcmState) &&
                            plainPcm.pcm->remainingBlocks == pcmState.pcm->remainingBlocks &&
                            plainPcm.pcm->lastTicks == pcmState.pcm->lastTicks &&
                            plainPcm.pcm->phase == pcmState.pcm->phase &&
                            plainPcm.pcm->completedBuffers == pcmState.pcm->completedBuffers &&
                            plainPcm.dspHardware->executedInstructions == pcmState.dspHardware->executedInstructions &&
                            plainPcm.dspHardware->dataRam == pcmState.dspHardware->dataRam &&
                            plainPcm.dspHardware->task->registers == pcmState.dspHardware->task->registers &&
                            plainPcm.dspHardware->task->dataTransfers == pcmState.dspHardware->task->dataTransfers &&
                            plainPcm.dspHardware->task->transferredBytes == pcmState.dspHardware->task->transferredBytes &&
                            plainPcm.dspHardware->task->returnAddresses == pcmState.dspHardware->task->returnAddresses &&
                            plainPcm.dspHardware->task->loops.size() == pcmState.dspHardware->task->loops.size() &&
                            plainPcm.externalInterrupt->delivered == pcmState.externalInterrupt->delivered &&
                            plainPcm.externalInterrupt->returned == pcmState.externalInterrupt->returned &&
                            plainPcm.decrementer->deadline == pcmState.decrementer->deadline &&
                            plainPcm.decrementer->armed == pcmState.decrementer->armed &&
                            plainPcm.decrementer->writes == pcmState.decrementer->writes &&
                            plainPcm.decrementer->delivered == pcmState.decrementer->delivered &&
                            plainPcm.decrementer->returned == pcmState.decrementer->returned &&
                            plainPcm.threadContextReturns == pcmState.threadContextReturns &&
                            plainPcm.externalInterrupt->vector == pcmState.externalInterrupt->vector &&
                            plainPcm.externalInterrupt->resume == pcmState.externalInterrupt->resume &&
                            plainPcm.externalInterrupt->savedMsr == pcmState.externalInterrupt->savedMsr &&
                            !plainPcm.externalInterrupt->inHandler &&
                            !plainPcm.externalInterrupt->active && plainPcm.externalInterrupt->suspended.empty() &&
                            plainResult.gameInitializationReturned &&
                            plainResult.gameLoopEntered && plainResult.firstLoopPreparationReturned &&
                            plainResult.sceneModuleLinked &&
                            plainResult.scenePrologEntered && plainResult.sceneInitializationEntered &&
                            plainResult.scenePrologReturned && plainResult.sceneConstructorsReturned == 28 &&
                            plainResult.systemTaskReturned && plainResult.taskPassesReturned == 3 &&
                            plainResult.deferredUpdatesReturned && plainResult.deferredUpdateQueueCleared &&
                            plainResult.screenPreparationReturned && plainResult.renderQueuePrepared &&
                            plainResult.renderPassReturned && plainResult.renderQueueCleared &&
                            plainResult.frameDrawSyncReturned &&
                            plainResult.postRenderCallbackReturned && plainResult.frameRetraceWaitReturned &&
                            plainResult.frameBufferExchangeReturned && plainResult.frameTimingReturned &&
                            plainResult.controllerUpdateReturned && plainResult.gameLoopIterationsCompleted == 1 &&
                            !dispatcher.Contains(sceneProlog) &&
                            plainResult.startupObjectsReturned == pcmResult.startupObjectsReturned &&
                            plainResult.resourceReadNotifications == pcmResult.resourceReadNotifications &&
                            plainPcm.discInterface->completedTransfers == pcmState.discInterface->completedTransfers,
                        "PCM observation changed CPU/RAM, DMA timing, original interrupts or native DSP execution.");
                const auto& observedGx = *pcmState.graphics;
                const auto& plainGx = *plainPcm.graphics;
                Require(observedGx.efb && plainGx.efb && observedGx.efb->lastCopy && plainGx.efb->lastCopy &&
                            observedGx.cp == plainGx.cp && observedGx.bp == plainGx.bp &&
                            observedGx.xf == plainGx.xf && observedGx.fifo == plainGx.fifo &&
                            observedGx.commands == plainGx.commands && observedGx.gathered == plainGx.gathered &&
                            observedGx.cpuBase == plainGx.cpuBase && observedGx.cpuEnd == plainGx.cpuEnd &&
                            observedGx.cpuWrite == plainGx.cpuWrite &&
                            observedGx.writeGatherAddress == plainGx.writeGatherAddress &&
                            observedGx.bpMask == plainGx.bpMask && observedGx.control == plainGx.control &&
                            observedGx.watermarkStatus == plainGx.watermarkStatus && observedGx.cpToken == plainGx.cpToken &&
                            observedGx.peControl == plainGx.peControl && observedGx.pePending == plainGx.pePending &&
                            observedGx.poke == plainGx.poke && observedGx.token == plainGx.token &&
                            observedGx.performanceSelect == plainGx.performanceSelect &&
                            observedGx.performanceBursts == plainGx.performanceBursts &&
                            observedGx.performanceCommands == plainGx.performanceCommands &&
                            observedGx.performanceResets == plainGx.performanceResets &&
                            observedGx.bursts == plainGx.bursts && observedGx.decodedCommands == plainGx.decodedCommands &&
                            observedGx.vertexCacheInvalidations == plainGx.vertexCacheInvalidations &&
                            observedGx.efb->color == plainGx.efb->color && observedGx.efb->depth == plainGx.efb->depth &&
                            observedGx.efb->defined == plainGx.efb->defined && observedGx.efb->copies == plainGx.efb->copies &&
                            observedGx.efb->clears == plainGx.efb->clears &&
                            observedGx.efb->undefinedCopies == plainGx.efb->undefinedCopies &&
                            observedGx.efb->lastCopy->address == plainGx.efb->lastCopy->address &&
                            observedGx.efb->lastCopy->width == plainGx.efb->lastCopy->width &&
                            observedGx.efb->lastCopy->height == plainGx.efb->lastCopy->height &&
                            observedGx.efb->lastCopy->stride == plainGx.efb->lastCopy->stride &&
                            observedGx.efb->lastCopy->undefinedPixels == plainGx.efb->lastCopy->undefinedPixels,
                        "First-loop observation changed GX/FIFO state or original EFB/XFB copy effects.");
                const auto& observedVi = *pcmState.video;
                const auto& plainVi = *plainPcm.video;
                Require(observedVi.registers == plainVi.registers && observedVi.lastTicks == plainVi.lastTicks &&
                            observedVi.elapsedTicks == plainVi.elapsedTicks && observedVi.fields == plainVi.fields &&
                            observedVi.writes == plainVi.writes && observedVi.resets == plainVi.resets &&
                            observedVi.clockStarted == plainVi.clockStarted,
                        "First-loop observation changed VI timing, interrupts or framebuffer register state.");
                auto failedScene = CreateOriginalStartupState(image);
                InstallOriginalStartupDisc(failedScene, disc);
                failedScene.pcm.emplace();
                uint64_t failedTicks = 1000000, failedFrames = 0;
                bindPcmTest(failedScene, failedTicks, failedFrames);
                const auto rejectedScene = RunOriginalStartup(dispatcher, failedScene, 10000000, 128,
                    [&](uint32_t pc) {
                        if (pc == sceneProlog) throw std::runtime_error("Injected scene observer failure.");
                    });
                Require(rejectedScene.reason == "Injected scene observer failure." &&
                            rejectedScene.sceneModuleLinked && rejectedScene.scenePrologEntered &&
                            rejectedScene.sceneConstructorsReturned == 0 &&
                            !rejectedScene.sceneInitializationEntered && !rejectedScene.scenePrologReturned &&
                            !dispatcher.Contains(sceneProlog) && !failedScene.callGuest && !failedScene.callException,
                        "Scene execution failure leaked native bindings or fabricated initialization.");
                auto interruptedRender = StartupFixture(image);
                InstallOriginalStartupDisc(*interruptedRender, disc);
                interruptedRender->pcm.emplace();
                uint64_t interruptedTicks = 1000000, interruptedFrames = 0;
                bindPcmTest(*interruptedRender, interruptedTicks, interruptedFrames);
                const auto rejectedRender = RunOriginalStartup(dispatcher, *interruptedRender, 50000000, 128,
                    [&](uint32_t pc) {
                        if (pc == 0x80181e7cu) throw std::runtime_error("Injected render observer failure.");
                    });
                Require(rejectedRender.reason == "Injected render observer failure." &&
                            rejectedRender.renderQueuePrepared && !rejectedRender.renderPassReturned &&
                            !rejectedRender.renderQueueCleared && !rejectedRender.frameDrawSyncReturned &&
                            !rejectedRender.postRenderCallbackReturned && !rejectedRender.frameRetraceWaitReturned &&
                            !rejectedRender.frameBufferExchangeReturned && !rejectedRender.frameTimingReturned &&
                            !rejectedRender.controllerUpdateReturned && rejectedRender.gameLoopIterationsCompleted == 0 &&
                            !dispatcher.Contains(sceneProlog) &&
                            !interruptedRender->callGuest && !interruptedRender->callException,
                        "Interrupted original render pass fabricated completion or leaked native scene bindings.");
                auto damagedLoop = StartupFixture(image);
                InstallOriginalStartupDisc(*damagedLoop, disc);
                damagedLoop->pcm.emplace();
                uint64_t damagedLoopTicks = 1000000, damagedLoopFrames = 0;
                bindPcmTest(*damagedLoop, damagedLoopTicks, damagedLoopFrames);
                ppc_write_u32(*damagedLoop, 0x8000ad3cu, 0);
                const auto rejectedLoop = RunOriginalStartup(dispatcher, *damagedLoop, 50000000);
                Require(rejectedLoop.stop == OriginalStartupStop::Dependency && rejectedLoop.pc == 0x8000ad3cu &&
                            rejectedLoop.reason.find("code mismatch") != std::string::npos &&
                            rejectedLoop.frameBufferExchangeReturned && rejectedLoop.frameTimingReturned &&
                            !rejectedLoop.controllerUpdateReturned && rejectedLoop.gameLoopIterationsCompleted == 0 &&
                            DescribeOriginalStartupStop(rejectedLoop).find("no complete game-loop iteration") !=
                                std::string::npos &&
                            !dispatcher.Contains(sceneProlog) && !damagedLoop->callGuest && !damagedLoop->callException,
                        "Corrupt original loop backedge fabricated a completed iteration or leaked scene bindings.");
                auto damagedLink = CreateOriginalStartupState(image);
                InstallOriginalStartupDisc(damagedLink, disc);
                damagedLink.pcm.emplace();
                uint64_t linkTicks = 1000000, linkFrames = 0;
                bindPcmTest(damagedLink, linkTicks, linkFrames);
                ppc_write_u32(damagedLink, 0x801b4860u, 0);
                const auto rejectedLink = RunOriginalStartup(dispatcher, damagedLink, 10000000);
                Require(rejectedLink.stop == OriginalStartupStop::Dependency &&
                            rejectedLink.pc == 0x801b4860u &&
                            rejectedLink.reason.find("code mismatch") != std::string::npos &&
                            rejectedLink.firstLoopPreparationReturned && !rejectedLink.sceneModuleLinked &&
                            rejectedLink.resourceReadNotifications == 2,
                        "Corrupt original OSLink fabricated scene-module link completion.");
                auto damagedCamera = CreateOriginalStartupState(image);
                InstallOriginalStartupDisc(damagedCamera, disc);
                damagedCamera.pcm.emplace();
                uint64_t damagedTicks = 1000000, damagedFrames = 0;
                bindPcmTest(damagedCamera, damagedTicks, damagedFrames);
                ppc_write_u32(damagedCamera, 0x80086044u, 0);
                const auto damagedObject = RunOriginalStartup(dispatcher, damagedCamera, 10000000);
                Require(damagedObject.stop == OriginalStartupStop::Dependency &&
                            damagedObject.pc == 0x80086044u && damagedObject.gameInitializationReturned &&
                            damagedObject.reason.find("code mismatch") != std::string::npos &&
                            damagedObject.startupObjectsReturned == 0 && damagedObject.resourceReadNotifications == 0 &&
                            !damagedObject.gameLoopEntered && !damagedObject.firstLoopPreparationReturned &&
                            !damagedCamera.callGuest && !damagedCamera.callException,
                        "Corrupt original camera code fabricated constructor or resource-read completion.");
                auto damagedLate = CreateOriginalStartupState(image);
                InstallOriginalStartupDisc(damagedLate, disc);
                damagedLate.pcm.emplace();
                uint64_t lateTicks = 1000000, lateFrames = 0;
                bindPcmTest(damagedLate, lateTicks, lateFrames);
                ppc_write_u32(damagedLate, 0x800154a0u, 0);
                const auto rejectedLate = RunOriginalStartup(dispatcher, damagedLate, 10000000);
                Require(rejectedLate.stop == OriginalStartupStop::Dependency &&
                            rejectedLate.pc == 0x800154a0u && rejectedLate.startupObjectsReturned == 8 &&
                            rejectedLate.reason.find("code mismatch") != std::string::npos &&
                            !rejectedLate.gameLoopEntered && !rejectedLate.firstLoopPreparationReturned &&
                            !damagedLate.callGuest && !damagedLate.callException,
                        "Corrupt late constructor fabricated main-loop entry or preparation.");
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
            auto damagedTask = CreateOriginalStartupState(image);
            InstallOriginalStartupDisc(damagedTask, disc);
            uint64_t damagedTaskTicks = 1000000;
            damagedTask.readTimeBase = [&] {
                return damagedTaskTicks += (*damagedTask.aiControl & 1u) ? 8 : 1000;
            };
            damagedTask.writeConsole = [](const std::string&) {};
            ppc_write_u16(damagedTask, 0x802e3da0u + 0x20, 0);
            const auto rejectedTask = RunOriginalStartup(dispatcher, damagedTask);
            const auto& rejectedDsp = *damagedTask.dspHardware;
            Require(rejectedTask.stop == OriginalStartupStop::Dependency &&
                        rejectedTask.dspInitReturned && !rejectedTask.dspTaskAdded &&
                        !rejectedTask.audioTaskInitReturned &&
                        rejectedTask.reason == "Native DSP task upload differs from build-time compiled DOL microcode." &&
                        rejectedDsp.task->mode == NativeDspTaskMode::LoaderValue &&
                        rejectedDsp.task->instructionUploads == 0 && rejectedDsp.task->uploadedBytes == 0 &&
                        rejectedDsp.task->cpuMailConsumed == 9 && rejectedDsp.task->interrupts == 0 &&
                        rejectedDsp.sentMail == 2 && rejectedDsp.consumedMail == 2 &&
                        rejectedDsp.completedPrograms == 1 && rejectedDsp.executedInstructions == 16404 &&
                        ppc_read_u32(damagedTask, 0x80349a98u) == 0 &&
                        std::all_of(rejectedDsp.task->instructionRam.begin(), rejectedDsp.task->instructionRam.end(),
                                    [](uint16_t word) { return word == 0; }),
                    "Changed original DSP task data fabricated upload, execution, interrupt or CPU init completion.");
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
