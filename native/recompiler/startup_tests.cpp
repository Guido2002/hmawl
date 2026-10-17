#include "dol_image.h"
#include "sha1.h"
#include "generated_us_startup.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestEntryAndRegisters(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    std::fill(std::begin(state.gpr), std::end(state.gpr), 0xdeadbeefu);
    state.lr = 0x80003158u;
    ppc_800032B0(state);
    Require(state.pc == state.lr, "Register initialization did not return to entry.");
    Require(state.gpr[1] == 0x8035cfa0u && state.gpr[2] == 0x80351e40u &&
                state.gpr[13] == 0x8034e9c0u,
            "Original startup produced incorrect stack or SDA registers.");
    for (unsigned reg = 0; reg < 32; ++reg) {
        if (reg != 1 && reg != 2 && reg != 13) {
            Require(state.gpr[reg] == 0, "Original startup did not clear a register.");
        }
    }
    std::cout << "Original register initialization passed; stack and SDA registers verified.\n";
}

void TestStartupHelpers(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.lr = 0x80001000u;
    for (uint16_t buttons : {uint16_t{0}, uint16_t{0xeef}, uint16_t{0xffff}}) {
        ppc_write_u16(state, 0x800030e4u, buttons);
        const uint32_t stack = state.gpr[1];
        unsigned resets = 0;
        state.callGuest = [&resets](uint32_t address, PpcState& guest) {
            Require(address == 0x801b568cu && guest.gpr[3] == 0 &&
                        guest.gpr[4] == 0 && guest.gpr[5] == 0,
                    "Reset check called an unexpected service or passed wrong arguments.");
            ++resets;
        };
        ppc_80003100(state);
        Require(resets == static_cast<unsigned>((buttons & 0xeef) == 0xeef) &&
                    state.gpr[1] == stack && state.pc == 0x80001000u,
                "Original pad/reset helper failed or damaged its stack/return address.");
    }
    ppc_80003140(state);
    ppc_8000314C(state);
    Require(state.gpr[3] == 1 &&
                ppc_read_u8(state, state.gpr[13] - 0x5338u) == 1,
            "Original SDA flag setter/getter failed.");
    for (uint32_t count : {0u, 1u, 17u, 257u}) {
        for (uint32_t i = 0; i < count; ++i) {
            ppc_write_u8(state, 0x80001000u + i, static_cast<uint8_t>(i ^ 0xa5));
        }
        ppc_write_u8(state, 0x80002000u, 0xee);
        state.gpr[3] = 0x80002000u;
        state.gpr[4] = 0x80001000u;
        state.gpr[5] = count;
        ppc_80003488(state);
        for (uint32_t i = 0; i < count; ++i) {
            Require(ppc_read_u8(state, 0x80002000u + i) ==
                        static_cast<uint8_t>(i ^ 0xa5), "Original startup byte copy failed.");
        }
        Require(count != 0 || ppc_read_u8(state, 0x80002000u) == 0xee,
                "Zero-length byte copy wrote memory.");
    }
    std::cout << "US pad/reset helper (test callback only), SDA flag and byte-copy helpers passed.\n";
}

void TestFloatingPointInitialization(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    Require(ppc_read_u64(state, 0x803495d0u) == 0,
            "Original FPR initialization constant is not double-precision zero.");
    for (unsigned reg = 0; reg < 32; ++reg) {
        state.fpr[reg] = {UINT64_C(0x7ff0000000000001) + reg,
                          UINT64_C(0x8000000000000000) + reg};
    }
    const auto before = state.memory;
    auto expected = state.fpr;
    for (auto& reg : expected) {
        reg.ps0 = 0;
    }
    state.fpscr = 0xfedcba98u;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.lr = 0x80003418u;
    const auto registers = std::vector<uint32_t>(std::begin(state.gpr), std::end(state.gpr));
    ppc_801B0BCC(state);
    Require(state.fpr == expected && state.fpscr == 0 &&
                state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                state.lr == 0x80003418u && state.pc == state.lr &&
                state.memory == before &&
                std::equal(registers.begin(), registers.end(), std::begin(state.gpr)),
            "Original scalar FPR initialization damaged paired lanes, flags or memory.");
    std::cout << "Original scalar FPR initialization clears all 32 primary lanes; "
                 "paired lanes preserved; original mtfsf clears FPSCR and returns.\n";
}

void TestPairedInitialization(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.gqr.fill(0xffffffffu);
    const auto memory = state.memory;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 0xfedcba98u;
    ppc_801B15A4(state);
    Require(std::all_of(state.gqr.begin(), state.gqr.end(),
                        [](uint32_t value) { return value == 0; }) &&
                state.pc == 0x801b15c8u && state.memory == memory &&
                state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                state.fpscr == 0xfedcba98u,
            "Original quantization initialization did not clear all GQRs or changed unrelated state.");
    Require(ppc_read_u64(state, 0x803495d8u) == 0,
            "Original paired initialization constant is not two single-precision zeros.");
    for (unsigned reg = 0; reg < 32; ++reg) {
        state.fpr[reg] = {UINT64_C(0x7ff0000000000001) + reg,
                          UINT64_C(0xfff8000000000000) + reg};
    }
    const auto gqr = state.gqr;
    const uint32_t stack = state.gpr[1];
    state.lr = 0x80003418u;
    ppc_801B0B44(state);
    Require(std::all_of(state.fpr.begin(), state.fpr.end(),
                        [](const PpcFpr& value) { return value == PpcFpr{0, 0}; }) &&
                state.fpscr == 0 && state.pc == state.lr &&
                state.gpr[1] == stack && state.gpr[2] == 0x80351e40u &&
                state.gpr[13] == 0x8034e9c0u && state.cr == 0x12345678u &&
                state.xer == 0xe0000000u && state.gqr == gqr && state.memory == memory,
            "Original paired/scalar FPR initialization did not clear both lanes and FPSCR.");
    std::cout << "Original GQR reset and paired/scalar FPR initialization body passed; "
                 "all 64 lanes and FPSCR zeroed, no RAM writes. Privileged prefix not executed.\n";
}

void TestHardwareFprInitialization(const DolImage& image) {
    for (bool paired : {false, true}) {
        auto state = image.CreateGameCubeState();
        ppc_800032B0(state);
        state.hid2 = paired ? 0xa0000000u : 0u;
        state.externalInterruptsEnabled = paired;
        state.gqr.fill(0);
        state.lr = 0x80003418u;
        state.fpscr = 0xfedcba98u;
        state.cr = 0x12345678u;
        for (unsigned reg = 0; reg < 32; ++reg) {
            state.fpr[reg] = {UINT64_C(0x7ff0000000000001) + reg,
                              UINT64_C(0xfff8000000000000) + reg};
        }
        auto expected = state.fpr;
        for (auto& reg : expected) {
            reg.ps0 = 0;
            if (paired) reg.ps1 = 0;
        }
        const auto memory = state.memory;
        const uint32_t stack = state.gpr[1];
        ppc_801B0B2C(state);
        Require(state.fpr == expected && state.fpscr == 0 &&
                    ppc_read_msr(state) == (paired ? 0xa000u : 0x2000u) &&
                    state.externalInterruptsEnabled == paired &&
                    state.hid2 == (paired ? 0xa0000000u : 0u) &&
                    state.gpr[1] == stack && state.gpr[2] == 0x80351e40u &&
                    state.gpr[13] == 0x8034e9c0u && state.memory == memory &&
                    state.pc == state.lr && state.cr == (paired ? 0x42345678u : 0x22345678u),
                "Full original FPR initializer failed MSR/HID2 selection or damaged state.");
    }
    std::cout << "Full original __OSFPRInit passed scalar/paired HID2 selection and MSR FP enable.\n";
}

void RunHardwareDependency(uint32_t address, PpcState& state) {
    switch (address) {
    case 0x801b0a6cu: ppc_801B0A6C(state); break;
    case 0x801b0a74u: ppc_801B0A74(state); break;
    case 0x801b0a7cu: ppc_801B0A7C(state); break;
    case 0x801b0a84u: ppc_801B0A84(state); break;
    case 0x801b0a8cu: ppc_801B0A8C(state); break;
    case 0x801b0a94u: ppc_801B0A94(state); break;
    case 0x801b0afcu: ppc_801B0AFC(state); break;
    case 0x801b0b24u: ppc_801B0B24(state); break;
    case 0x801b20e4u: ppc_801B20E4(state); break;
    case 0x801b20ecu: ppc_801B20EC(state); break;
    case 0x801b11b8u: ppc_801B11B8(state); break;
    case 0x801b6988u: ppc_801B6988(state); break;
    case 0x801b1618u: ppc_801B1618(state); break;
    case 0x801b147cu: ppc_801B147C(state); break;
    case 0x801b5550u: ppc_801B5550(state); break;
    case 0x801b4c90u: ppc_801B4C90(state); break;
    case 0x801b1460u: ppc_801B1460(state); break;
    case 0x801d33c8u: ppc_801D33C8(state); break;
    case 0x801b2428u: ppc_801B2428(state); break;
    case 0x801b24acu: ppc_801B24AC(state); break;
    case 0x800055ccu: ppc_800055CC(state); break;
    case 0x801b7bf0u: ppc_801B7BF0(state); break;
    case 0x801b7c10u: ppc_801B7C10(state); break;
    case 0x801b1584u: ppc_801B1584(state); break;
    case 0x801b0ae4u: ppc_801B0AE4(state); break;
    case 0x801b0aecu: ppc_801B0AEC(state); break;
    case 0x801b24e0u: ppc_801B24E0(state); break;
    case 0x801b0b2cu: ppc_801B0B2C(state); break;
    case 0x801b29dcu: ppc_801B29DC(state); break;
    case 0x801b0a28u: ppc_801B0A28(state); break;
    case 0x801b0a30u: ppc_801B0A30(state); break;
    case 0x801b24f0u: ppc_801B24F0(state); break;
    case 0x801b2388u: ppc_801B2388(state); break;
    case 0x801b0a18u: ppc_801B0A18(state); break;
    case 0x801b0a20u: ppc_801B0A20(state); break;
    case 0x801b0a38u: ppc_801B0A38(state); break;
    case 0x801b0a40u: ppc_801B0A40(state); break;
    case 0x801b27e4u: ppc_801B27E4(state); break;
    case 0x801b34ecu: ppc_801B34EC(state); break;
    case 0x801b3a44u: ppc_801B3A44(state); break;
    case 0x801b3a6cu: ppc_801B3A6C(state); break;
    case 0x801d33e4u: ppc_801D33E4(state); break;
    default:
        throw std::runtime_error("Unsupported original hardware dependency: " +
                                 std::to_string(address));
    }
    Require(state.pc == state.lr, "Original hardware dependency did not return.");
}

void TestOriginalSystemTime(const DolImage& image) {
    for (bool interrupts : {false, true}) {
        for (uint64_t ticks : {UINT64_C(0), UINT64_C(0x12345678ffffffff), UINT64_MAX}) {
            for (uint64_t offset : {UINT64_C(0), UINT64_C(1),
                                    UINT64_C(0xfedcba9876543210)}) {
                auto state = image.CreateGameCubeState();
                ppc_800032B0(state);
                state.externalInterruptsEnabled = interrupts;
                state.msr = 0x2030u;
                state.lr = 0x80001000u;
                state.gpr[29] = 0x29292929u;
                state.gpr[30] = 0x30303030u;
                state.gpr[31] = 0x31313131u;
                state.fpr.fill({UINT64_MAX, UINT64_MAX});
                const auto fpr = state.fpr;
                const uint32_t stack = state.gpr[1];
                ppc_write_u64(state, 0x800030d8u, offset);
                const auto before = state.memory;
                unsigned reads = 0;
                state.readTimeBase = [&] {
                    Require(!state.externalInterruptsEnabled,
                            "Original system-time read did not mask interrupts.");
                    Require(++reads <= 3, "Stable time-base read unexpectedly retried.");
                    return ticks;
                };
                state.callGuest = RunHardwareDependency;
                ppc_801B7C10(state);
                const uint64_t actual = (static_cast<uint64_t>(state.gpr[3]) << 32) |
                                         state.gpr[4];
                Require(actual == ticks + offset && reads == 3 &&
                            state.externalInterruptsEnabled == interrupts &&
                            state.msr == 0x2030u && state.pc == state.lr &&
                            state.gpr[1] == stack && state.gpr[29] == 0x29292929u &&
                            state.gpr[30] == 0x30303030u && state.gpr[31] == 0x31313131u &&
                            state.fpr == fpr,
                        "Original system time lost carry/wrap, interrupts or return ABI.");
                const auto frame = ppc_memory_offset(state, stack - 0x20, 0x28);
                for (size_t i = 0; i < before.size(); ++i) {
                    if (i >= frame && i < frame + 0x28) continue;
                    Require(state.memory[i] == before[i],
                            "Original system time wrote outside its stack frame.");
                }
            }
        }
    }
    auto state = image.CreateGameCubeState();
    state.lr = 0x80001000u;
    const uint64_t samples[] = {UINT64_C(0xffffffff), UINT64_C(0x100000000),
        UINT64_C(0x100000000), UINT64_C(0x100000001),
        UINT64_C(0x100000002), UINT64_C(0x100000003)};
    size_t sample = 0;
    state.readTimeBase = [&] {
        Require(sample < 6, "Original OSGetTime failed to converge after rollover.");
        return samples[sample++];
    };
    ppc_801B7BF0(state);
    Require(sample == 6 && state.gpr[3] == 1 && state.gpr[4] == 2 &&
                state.pc == state.lr,
            "Original OSGetTime did not retry a torn high/low time-base sample.");
    std::cout << "Original OSGetTime/__OSGetSystemTime passed rollover retry, "
                 "64-bit offset carry/wrap, masked sampling and interrupt/ABI restoration.\n";
}

void TestCompleteHardwareInitialization(const DolImage& image) {
    for (uint32_t cache : {0u, 0x4000u, 0x8000u, 0xc000u}) {
        for (bool l2Enabled : {false, true}) {
            for (bool interrupts : {false, true}) {
                auto state = image.CreateGameCubeState();
                ppc_800032B0(state);
                // This fixture starts with the loader's address translation enabled.
                state.msr = 0x30u;
                state.externalInterruptsEnabled = interrupts;
                state.hid0 = cache;
                state.l2cr = l2Enabled ? 0x80000000u : 0u;
                state.gqr.fill(UINT32_MAX);
                state.fpr.fill({UINT64_MAX, UINT64_MAX});
                state.fpscr = UINT32_MAX;
                state.lr = 0x8000315cu;
                state.gpr[30] = 0x13579bdfu;
                constexpr uint32_t errorSlot = 0x80317a94u;
                ppc_write_u32(state, errorSlot, 0x80001000u);
                const auto before = state.memory;
                const uint32_t stack = state.gpr[1];
                std::vector<uint32_t> debugMessages;
                unsigned invalidations = 0, handlers = 0, maskedWrites = 0;
                unsigned calls = 0;
                state.callGuest = [&](uint32_t address, PpcState& guest) {
                    Require(++calls < 100, "Hardware fixture exceeded its dependency budget.");
                    if (address == 0x801b27e4u) {
                        Require(ppc_read_msr(guest) == 0x30u &&
                                    (guest.l2cr & 0x80000000u) == 0,
                                "L2 invalidation did not disable FP/EE and L2 first.");
                        ++invalidations;
                    } else if (address == 0x801b0a40u &&
                               (guest.gpr[3] & 0x00200000u) != 0) {
                        ++maskedWrites;
                        Require(!guest.externalInterruptsEnabled,
                                "L2 invalidation was not performed with interrupts masked.");
                    } else if (address == 0x801b34ecu) {
                        Require(guest.gpr[3] == 1 && guest.gpr[4] == 0x801b287cu,
                                "Cache initialization installed the wrong DMA error handler.");
                        ++handlers;
                    } else if (address == 0x801d33e4u) {
                        debugMessages.push_back(guest.gpr[3]);
                    }
                    RunHardwareDependency(address, guest);
                    if (address == 0x801b34ecu) {
                        Require(guest.gpr[3] == 0x80001000u,
                                "Error-handler registration lost the previous handler.");
                    }
                };
                ppc_80003400(state);
                std::vector<uint32_t> expectedMessages;
                if ((cache & 0x8000u) == 0) expectedMessages.push_back(0x802e0454u);
                if ((cache & 0x4000u) == 0) expectedMessages.push_back(0x802e0470u);
                if (!l2Enabled) expectedMessages.push_back(0x802e048cu);
                expectedMessages.push_back(0x802e04a4u);
                Require(debugMessages == expectedMessages && handlers == 1 &&
                            invalidations == (l2Enabled ? 0u : 1u) &&
                            maskedWrites == (l2Enabled ? 0u : 1u) &&
                            state.hid0 == 0xc000u && state.hid2 == 0xa0000000u &&
                            state.l2cr == 0x80000000u &&
                            ppc_read_msr(state) == (interrupts ? 0xa030u : 0x2030u) &&
                            state.externalInterruptsEnabled == interrupts &&
                            state.gpr[1] == stack && state.gpr[2] == 0x80351e40u &&
                            state.gpr[13] == 0x8034e9c0u &&
                            state.gpr[30] == 0x13579bdfu &&
                            state.gpr[31] == 0x8000315cu &&
                            state.pc == 0x8000315cu && state.lr == state.pc &&
                            state.fpscr == 0 &&
                            ppc_read_u32(state, errorSlot) == 0x801b287cu &&
                            std::all_of(state.gqr.begin(), state.gqr.end(),
                                        [](uint32_t v) { return v == 0; }) &&
                            std::all_of(state.fpr.begin(), state.fpr.end(),
                                        [](const PpcFpr& v) { return v == PpcFpr{0, 0}; }),
                        "Complete original hardware initialization failed state/return checks.");
                const size_t frame = ppc_memory_offset(state, stack - 0xa0u, 0xa8);
                const size_t slot = ppc_memory_offset(state, errorSlot, 4);
                for (size_t i = 0; i < before.size(); ++i) {
                    if ((i >= frame && i < frame + 0xa8) || (i >= slot && i < slot + 4))
                        continue;
                    Require(state.memory[i] == before[i],
                            "Hardware initialization wrote outside stack/error-table guards.");
                }
            }
        }
    }
    std::cout << "Complete original __init_hardware returns to 0x8000315c: paired/FPR/L1/L2 "
                 "initialization and DMA error-handler registration passed all 16 initial "
                 "cache/interrupt combinations. Loader IR/DR precondition; not a game boot.\n";
}

void TestOriginalCacheControls(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.lr = 0x80001000u;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 0xf1234567u;
    const auto memory = state.memory;
    const uint32_t stack = state.gpr[1];
    for (uint32_t initial : {0u, 0x4000u, 0x8000u, 0xc000u}) {
        state.hid0 = initial;
        ppc_801B0A28(state);
        Require(state.gpr[3] == initial && state.pc == state.lr,
                "Original HID0 getter did not read cache state.");
        state.gpr[3] = initial | 0x800u;
        ppc_801B0A30(state);
        Require(state.hid0 == initial && state.pc == state.lr,
                "Original HID0 setter did not complete instruction-cache invalidation.");
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            ppc_801B24E0(state);
            Require(state.hid0 == initial && state.gpr[3] == (initial | 0x800u) &&
                        state.pc == state.lr,
                    "Original ICFlashInvalidate changed cache enables or left ICFI set.");
        }
        ppc_801B24F0(state);
        Require(state.hid0 == (initial | 0x8000u) && state.pc == state.lr,
                "Original ICEnable failed to preserve data-cache state.");
        ppc_801B2388(state);
        Require(state.hid0 == 0xc000u && state.pc == state.lr &&
                    state.memory == memory && state.gpr[1] == stack &&
                    state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                    state.fpscr == 0xf1234567u,
                "Original DCEnable failed or cache controls damaged RAM/ABI/flags.");
    }
    std::cout << "Original HID0 wrappers, ICFlashInvalidate, ICEnable and DCEnable passed; "
                 "ICFI self-clears and coherent RAM/flags/stack are preserved.\n";
}

void TestOriginalFprSaveRestore(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    constexpr uint32_t end = 0x80002000u;
    state.gpr[11] = end;
    state.lr = 0x80003000u;
    state.fpscr = 0xa1234567u;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    for (unsigned reg = 0; reg < 32; ++reg) {
        state.fpr[reg] = {UINT64_C(0x7ff0000000000001) + reg,
                          UINT64_C(0x3ff0000000000000) + reg};
    }
    const auto original = state.fpr;
    auto expectedMemory = state.memory;
    for (unsigned reg = 14; reg < 32; ++reg) {
        const uint32_t address = end - (32u - reg) * 8u;
        for (unsigned byte = 0; byte < 8; ++byte) {
            expectedMemory[ppc_memory_offset(state, address + byte, 1)] =
                static_cast<uint8_t>(original[reg].ps0 >> (56u - byte * 8u));
        }
    }
    ppc_80235E00(state);
    Require(state.memory == expectedMemory && state.fpr == original &&
                state.pc == state.lr && state.gpr[11] == end &&
                state.fpscr == 0xa1234567u && state.cr == 0x12345678u,
            "Original FPR save helper changed the ABI, flags or memory guards.");
    for (unsigned reg = 14; reg < 32; ++reg) {
        state.fpr[reg].ps0 = UINT64_C(0xdeadbeefdeadbeef);
        state.fpr[reg].ps1 ^= UINT64_C(0xffffffffffffffff);
    }
    auto expected = state.fpr;
    for (unsigned reg = 14; reg < 32; ++reg) {
        expected[reg].ps0 = original[reg].ps0;
    }
    ppc_80235E4C(state);
    Require(state.fpr == expected && state.memory == expectedMemory &&
                state.pc == state.lr && state.gpr[11] == end &&
                state.fpscr == 0xa1234567u && state.cr == 0x12345678u &&
                state.xer == 0xe0000000u,
            "Original FPR restore helper changed paired lanes, volatile FPRs, memory or flags.");
    std::cout << "Original f14..f31 save/restore helpers preserve exact NaN bits, "
                 "paired lanes and RAM guards.\n";
}

void TestOriginalFpscrAccessors(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.lr = 0x80003000u;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.ctr = 0x87654321u;
    for (unsigned reg = 0; reg < 32; ++reg) {
        state.fpr[reg] = {UINT64_C(0xfff0123456780000) + reg,
                          UINT64_C(0x3ff0000000000000) + reg};
    }
    const auto fpr = state.fpr;
    const uint32_t stack = state.gpr[1];
    for (uint32_t value : {0u, 1u, 2u, 3u, 4u, 0xffffffffu,
                           0x60000000u, 0x80000000u, 0x01000080u}) {
        state.gpr[3] = value;
        const auto before = state.memory;
        ppc_801B0ABC(state);
        const uint32_t expected = value == 0xffffffffu ? 0xffffffffu :
                                  value == 0x60000000u ? 0u :
                                  value == 0x01000080u ? 0x61000080u : value;
        Require(state.fpscr == expected && state.fpr == fpr &&
                    state.gpr[1] == stack && state.pc == state.lr &&
                    state.gpr[3] == value && state.gpr[4] == 0 &&
                    state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                    state.ctr == 0x87654321u,
                "Original FPSCR setter lost summaries, FPRs or its ABI.");
        const auto frame = ppc_memory_offset(state, stack - 0x20u, 0x20);
        Require(std::equal(before.begin(), before.begin() + frame, state.memory.begin()) &&
                    std::equal(before.begin() + frame + 0x20, before.end(),
                               state.memory.begin() + frame + 0x20),
                "Original FPSCR setter wrote outside its stack frame.");
        const auto beforeRead = state.memory;
        ppc_801B0A9C(state);
        Require(state.gpr[3] == expected && state.fpscr == expected &&
                    state.fpr == fpr && state.gpr[1] == stack &&
                    state.pc == state.lr && state.cr == 0x12345678u &&
                    state.xer == 0xe0000000u && state.ctr == 0x87654321u,
                "Original FPSCR getter changed FPSCR, callee-saved FPRs or its ABI.");
        const auto readFrame = ppc_memory_offset(state, stack - 0x18u, 0x18);
        Require(std::equal(beforeRead.begin(), beforeRead.begin() + readFrame,
                           state.memory.begin()) &&
                    std::equal(beforeRead.begin() + readFrame + 0x18, beforeRead.end(),
                               state.memory.begin() + readFrame + 0x18),
                "Original FPSCR getter wrote outside its stack frame.");
    }
    std::cout << "Original SDK FPSCR read/write wrappers preserve f31, paired lanes, "
                 "stack guards and status bits across all rounding modes.\n";
}

void TestOriginalSpeculationDisable(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.fpr.fill({UINT64_MAX, UINT64_MAX});
    state.gqr.fill(0x12345678u);
    state.performanceMonitor.fill(0x87654321u);
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 0x80000003u;
    state.lr = 0x801b0e60u;
    const uint32_t stack = state.gpr[1];
    const auto before = state;
    const size_t frame = ppc_memory_offset(state, stack - 8, 16);
    std::vector<uint32_t> calls;
    state.callGuest = [&](uint32_t address, PpcState& guest) {
        Require(calls.size() < 2, "Speculation wrapper exceeded dependency budget.");
        calls.push_back(address);
        RunHardwareDependency(address, guest);
    };
    for (bool interrupts : {false, true}) {
        state.externalInterruptsEnabled = interrupts;
        for (uint32_t initial : {0u, 0x200u, 0x4000u, 0x4200u,
                                 0x8000u, 0x8200u, 0xc000u, 0xc200u}) {
            // Preserve a loader bit without supporting changes to that bit.
            state.hid0 = initial | 0x00100000u;
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                calls.clear();
                ppc_801B0AFC(state);
                Require(calls == std::vector<uint32_t>{0x801b0a28u, 0x801b0a30u} &&
                            state.hid0 == (initial | 0x00100200u) &&
                            state.gpr[3] == state.hid0 && state.pc == state.lr &&
                            state.lr == before.lr && state.ctr == before.ctr &&
                            state.externalInterruptsEnabled == interrupts,
                        "Original speculation wrapper failed SPD, call order, return or interrupt state.");
                Require(state.cr == before.cr && state.xer == before.xer &&
                            state.fpscr == before.fpscr && state.fpr == before.fpr &&
                            state.gqr == before.gqr && state.msr == before.msr &&
                            state.hid2 == before.hid2 && state.l2cr == before.l2cr &&
                            state.performanceMonitor == before.performanceMonitor,
                        "Original speculation wrapper changed unrelated CPU state.");
                for (unsigned reg = 0; reg < 32; ++reg) {
                    if (reg == 0 || reg == 3) continue;
                    Require(state.gpr[reg] == before.gpr[reg],
                            "Original speculation wrapper damaged its return ABI.");
                }
                for (size_t i = 0; i < state.memory.size(); ++i) {
                    if (i >= frame && i < frame + 16) continue;
                    Require(state.memory[i] == before.memory[i],
                            "Original speculation wrapper wrote outside its stack frame.");
                }
            }
        }
    }
    std::cout << "Original PPCDisableSpeculation sets HID0 SPD through original wrappers; "
                 "cache bits, loader bits, interrupts and ABI preserved. No pipeline emulation.\n";
}

void TestOriginalNonIeeeMode(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.fpr.fill({UINT64_MAX, UINT64_MAX});
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.lr = 0x801b0e64u;
    const auto before = state.memory;
    for (uint32_t flags : {0u, 1u, 2u, 3u, 4u, 0x80000003u}) {
        state.fpscr = flags;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            ppc_801B0B24(state);
            Require(state.fpscr == (flags | 4u) && state.pc == state.lr &&
                        state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                        state.gpr[1] == 0x8035cfa0u && state.memory == before &&
                        std::all_of(state.fpr.begin(), state.fpr.end(),
                            [](const PpcFpr& fpr) {
                                return fpr == PpcFpr{UINT64_MAX, UINT64_MAX};
                            }),
                    "Original non-IEEE wrapper did not set only NI or preserve ABI.");
        }
    }
    std::cout << "Original PPCSetFpNonIEEEMode sets guest FPSCR NI and returns; "
                 "rounding bits, FPRs, RAM and flags preserved. No arithmetic implied.\n";
}

void TestArgumentRelocation(const DolImage& image) {
    for (unsigned path = 0; path < 4; ++path) {
        auto state = image.CreateGameCubeState();
        ppc_write_u32(state, 0x800000f4u, path == 0 ? 0u : 0x80001000u);
        ppc_write_u32(state, 0x80001008u, path < 2 ? 0u : 0x20u);
        ppc_write_u32(state, 0x80001020u, path < 3 ? 0u : 3u);
        for (uint32_t i = 0; i < 3; ++i) {
            ppc_write_u32(state, 0x80001024u + i * 4, 0x80u + i * 16u);
        }
        ppc_800031F8(state);
        Require(state.pc == 0x80003260u &&
                    state.gpr[14] == (path == 3 ? 3u : 0u) &&
                    state.gpr[15] == (path == 3 ? 0x80001024u : 0u),
                "Original argument setup selected an incorrect path or continuation.");
        if (path == 3) {
            Require(state.ctr == 0 && ppc_read_u32(state, 0x80000034u) == 0x80001020u,
                    "Original argument setup did not update CTR or arena boundary.");
            for (uint32_t i = 0; i < 3; ++i) {
                Require(ppc_read_u32(state, 0x80001024u + i * 4) ==
                            0x80001080u + i * 16u, "Argument pointer relocation failed.");
            }
        }
    }
    std::cout << "US argument-pointer relocation passed for absent, empty and populated arguments.\n";
}

void BindMemoryFunctions(PpcState& state) {
    state.callGuest = [](uint32_t address, PpcState& guest) {
        switch (address) {
        case 0x800055ccu:
            ppc_800055CC(guest);
            break;
        case 0x800054e4u:
            ppc_800054E4(guest);
            break;
        case 0x80005514u:
            ppc_80005514(guest);
            break;
        default:
            throw std::runtime_error("Unsupported startup memory dependency at " +
                                     std::to_string(address));
        }
    };
}

void TestOriginalCacheRanges(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.lr = 0x80001000u;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 4;
    const auto memory = state.memory;
    for (auto function : {ppc_801B2428, ppc_801B24AC}) {
        for (uint32_t alias : {0u, 0x80000000u, 0xc0000000u}) {
            for (uint32_t offset : {0u, 1u, 31u}) {
                for (uint32_t length : {0u, 1u, 31u, 32u, 33u, 97u}) {
                    state.gpr[3] = alias + 0x1000u + offset;
                    state.gpr[4] = length;
                    state.gpr[5] = 0x12345678u;
                    state.ctr = 0x87654321u;
                    const uint32_t blocks = (offset + length + 31u) / 32u;
                    function(state);
                    Require(state.pc == state.lr && state.gpr[1] == 0x8035cfa0u &&
                                state.memory == memory && state.xer == 0xe0000000u &&
                                state.fpscr == 4 &&
                                state.gpr[3] == alias + 0x1000u + offset +
                                    (length == 0 ? 0 : blocks * 32) &&
                                state.gpr[4] == (length == 0 ? 0 : blocks) &&
                                state.gpr[5] == (length == 0 ? 0x12345678u : offset) &&
                                state.ctr == (length == 0 ? 0x87654321u : 0),
                            "Original cache range failed alignment, loop count, zero length or ABI.");
                }
            }
        }
        state.gpr[3] = 0xcc000000u;
        state.gpr[4] = 0;
        function(state);
        Require(state.pc == state.lr && state.memory == memory,
                "Original zero-length cache range accessed an unmapped address.");
        state.gpr[4] = 1;
        bool rejected = false;
        try {
            function(state);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        Require(rejected && state.memory == memory,
                "Original nonempty MMIO cache range did not explicitly reject.");
    }
    std::cout << "Original DCFlushRangeNoSync/ICInvalidateRange passed RAM aliases, "
                 "unaligned ranges, loop counts, zero lengths and unmapped rejection.\n";
}

void ExpectExceptionInitialization(PpcState& expected, bool debugPresent,
                                   uint32_t debug, uint32_t marked) {
    if (ppc_read_u32(expected, 0x80000060u) == 0) {
        for (uint32_t i = 0; i < 0x24; ++i) {
            ppc_write_u8(expected, 0x80000060u + i,
                         ppc_read_u8(expected, 0x801b1438u + i));
        }
    }
    const uint32_t number = ppc_read_u32(expected, 0x801b14f8u);
    for (uint32_t exception = 0; exception < 15; ++exception) {
        const bool isMarked = (marked & (1u << exception)) != 0;
        if (debugPresent && debug >= 2 && isMarked) continue;
        ppc_write_u32(expected, 0x801b14f8u, number | exception);
        ppc_write_u32(expected, 0x801b14e8u,
                     isMarked ? ppc_read_u32(expected, 0x801b145cu) : 0x60000000u);
        const uint32_t vector = 0x80000000u +
            ppc_read_u32(expected, 0x802dfdacu + exception * 4);
        for (uint32_t i = 0; i < 0x98; ++i) {
            ppc_write_u8(expected, vector + i, ppc_read_u8(expected, 0x801b1490u + i));
        }
    }
    ppc_write_u32(expected, 0x803495e4u, 0x80003000u);
    for (uint32_t exception = 0; exception < 15; ++exception) {
        ppc_write_u32(expected, 0x80003000u + exception * 4, 0x801b152cu);
    }
    ppc_write_u32(expected, 0x801b14f8u, number);
}

void TestOriginalExceptionInitialization(const DolImage& image) {
    for (uint32_t debug : {0u, 1u, 2u, UINT32_MAX}) {
        for (uint32_t marked : {0u, 0x7fffu, 0x5555u, 0x80000000u}) {
            for (bool integratorPresent : {false, true}) {
                auto state = image.CreateGameCubeState();
                ppc_800032B0(state);
                ppc_801D3348(state);
                state.externalInterruptsEnabled = false;
                state.lr = 0x801b0f60u;
                state.cr = 0x12345678u;
                state.xer = 0xe0000000u;
                state.fpscr = 4;
                state.fpr.fill({UINT64_MAX, UINT64_MAX});
                ppc_write_u32(state, 0x803495c4u, debug == 0 ? 0 : 0x80004000u);
                ppc_write_u32(state, 0x80004000u, debug);
                ppc_write_u32(state, 0x80000044u, marked);
                std::fill(state.memory.begin() + 0x100, state.memory.begin() + 0x1800, 0xa5);
                ppc_write_u32(state, 0x80000060u, integratorPresent ? 0x12345678u : 0);
                const auto before = state;
                auto expected = state;
                ExpectExceptionInitialization(expected, debug != 0, debug, marked);
                unsigned calls = 0;
                std::vector<std::pair<uint32_t, uint32_t>> flushed, invalidated;
                state.callGuest = [&](uint32_t address, PpcState& guest) {
                    Require(++calls < 150, "Exception initialization exceeded dependency budget.");
                    if (address == 0x801b2428u) flushed.push_back({guest.gpr[3], guest.gpr[4]});
                    if (address == 0x801b24acu) invalidated.push_back({guest.gpr[3], guest.gpr[4]});
                    RunHardwareDependency(address, guest);
                };
                ppc_801B11B8(state);
                std::vector<std::pair<uint32_t, uint32_t>> ranges;
                if (!integratorPresent) ranges.push_back({0x80000060u, 0x24u});
                for (unsigned i = 0; i < 15; ++i) {
                    if (debug < 2 || (marked & (1u << i)) == 0) {
                        ranges.push_back({0x80000000u +
                            ppc_read_u32(before, 0x802dfdacu + i * 4), 0x98u});
                    }
                }
                Require(state.pc == before.lr && state.lr == before.lr &&
                            flushed == ranges && invalidated == ranges &&
                            !state.externalInterruptsEnabled &&
                            state.fpr == before.fpr && state.fpscr == before.fpscr &&
                            state.xer == before.xer,
                        "Original exception initializer failed return, cache-call count or CPU guards.");
                for (unsigned reg = 0; reg < 32; ++reg) {
                    if (reg == 0 || (reg >= 3 && reg <= 6)) continue;
                    Require(state.gpr[reg] == before.gpr[reg],
                            "Original exception initializer damaged preserved registers.");
                }
                const size_t frame = ppc_memory_offset(state, before.gpr[1] - 0xa8u, 0xb0);
                for (size_t i = 0; i < state.memory.size(); ++i) {
                    if (i >= frame && i < frame + 0xb0) continue;
                    Require(state.memory[i] == expected.memory[i],
                            "Original exception initializer wrote unexpected RAM or installed wrong vectors.");
                }
            }
        }
    }
    std::cout << "Original OSExceptionInit installs exact vector templates and default table; "
                 "debug ownership, marked masks, existing integrator, cache ranges and ABI checked. "
                 "Installed exception code is not executed.\n";
}

void ExpectSystemCallInstallation(PpcState& expected) {
    for (uint32_t i = 0; i < 0x1c; ++i) {
        ppc_write_u8(expected, 0x80000c00u + i,
                     ppc_read_u8(expected, 0x801b6968u + i));
    }
}

void TestOriginalSystemCallInstallation(const DolImage& image) {
    for (bool interrupts : {false, true}) {
        for (bool existingVector : {false, true}) {
            auto state = image.CreateGameCubeState();
            ppc_800032B0(state);
            state.externalInterruptsEnabled = interrupts;
            state.lr = 0x801b0f64u;
            state.cr = 0x12345678u;
            state.xer = 0xe0000000u;
            state.fpscr = 4;
            state.hid0 = 0xc200u;
            state.fpr.fill({UINT64_MAX, UINT64_MAX});
            state.gqr.fill(0x12345678u);
            state.performanceMonitor.fill(0x87654321u);
            std::fill(state.memory.begin() + 0xc00, state.memory.begin() + 0xd00,
                      existingVector ? 0xa5 : 0);
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                const auto before = state;
                auto expected = state;
                ExpectSystemCallInstallation(expected);
                std::vector<uint32_t> calls;
                state.callGuest = [&](uint32_t address, PpcState& guest) {
                    Require(calls.size() < 3, "System-call installation exceeded dependency budget.");
                    calls.push_back(address);
                    Require(guest.externalInterruptsEnabled == interrupts,
                            "System-call installation changed the interrupt mask.");
                    if (address == 0x800055ccu) {
                        Require(guest.gpr[3] == 0x80000c00u && guest.gpr[4] == 0x801b6968u &&
                                    guest.gpr[5] == 0x1c,
                                "System-call installation passed an incorrect vector copy range.");
                    } else {
                        Require(guest.gpr[3] == 0x80000c00u && guest.gpr[4] == 0x100,
                                "System-call installation passed an incorrect cache range.");
                    }
                    RunHardwareDependency(address, guest);
                };
                ppc_801B6988(state);
                Require(calls == std::vector<uint32_t>{0x800055ccu, 0x801b2428u, 0x801b24acu} &&
                            state.pc == before.lr && state.lr == before.lr &&
                            state.fpr == before.fpr && state.gqr == before.gqr &&
                            state.fpscr == before.fpscr && state.hid0 == before.hid0 &&
                            state.hid2 == before.hid2 && state.msr == before.msr &&
                            state.l2cr == before.l2cr &&
                            state.performanceMonitor == before.performanceMonitor &&
                            state.externalInterruptsEnabled == interrupts,
                        "Original system-call installer failed call order, return or CPU guards.");
                for (unsigned reg = 0; reg < 32; ++reg) {
                    if (reg == 0 || (reg >= 3 && reg <= 6)) continue;
                    Require(state.gpr[reg] == before.gpr[reg],
                            "System-call installer damaged a preserved integer register.");
                }
                const size_t frame = ppc_memory_offset(state, before.gpr[1] - 0x10, 0x18);
                for (size_t i = 0; i < state.memory.size(); ++i) {
                    if (i >= frame && i < frame + 0x18) continue;
                    Require(state.memory[i] == expected.memory[i],
                            "System-call installer changed RAM outside its vector and stack.");
                }
            }
        }
    }
    std::cout << "Original __OSInitSystemCall installs exact 0x1c-byte vector at 0x80000c00; "
                 "0x100-byte cache ranges, replacement/repetition and interrupt/ABI guards passed. "
                 "System-call vector execution remains unsupported.\n";
}

void TestOriginalResetRegistration(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.lr = 0x80001000u;
    constexpr uint32_t queue = 0x80349648u;
    ppc_write_u64(state, queue, 0);
    std::vector<std::pair<uint32_t, uint32_t>> registered;
    unsigned index = 0;
    for (uint32_t priority : {10u, 0u, 5u, 10u, UINT32_MAX, 1u, UINT32_MAX}) {
        const uint32_t node = 0x80004000u + index++ * 16;
        ppc_write_u32(state, node, 0x80001000u + index * 4);
        ppc_write_u32(state, node + 4, priority);
        ppc_write_u64(state, node + 8, UINT64_MAX);
        const auto before = state;
        auto expected = state;
        const auto position = std::find_if(registered.begin(), registered.end(),
            [&](const auto& entry) { return entry.second > priority; });
        registered.insert(position, {node, priority});
        ppc_write_u32(expected, queue, registered.front().first);
        ppc_write_u32(expected, queue + 4, registered.back().first);
        for (size_t i = 0; i < registered.size(); ++i) {
            ppc_write_u32(expected, registered[i].first + 8,
                i + 1 < registered.size() ? registered[i + 1].first : 0);
            ppc_write_u32(expected, registered[i].first + 12,
                i == 0 ? 0 : registered[i - 1].first);
        }
        state.gpr[3] = node;
        ppc_801B5550(state);
        Require(state.pc == state.lr && state.memory == expected.memory &&
                    state.gpr[3] == node && state.gpr[1] == before.gpr[1],
                "Original reset registration failed stable unsigned ordering or exact list writes.");
    }
    std::cout << "Original reset registration passed empty/head/middle/tail insertion, "
                 "stable equal priorities and unsigned maximum priority; callbacks not executed.\n";
}

void TestOriginalAlarmInitialization(const DolImage& image) {
    for (bool interrupts : {false, true}) {
        for (bool installed : {false, true}) {
            for (bool existingReset : {false, true}) {
                auto state = image.CreateGameCubeState();
                ppc_800032B0(state);
                state.externalInterruptsEnabled = interrupts;
                state.lr = 0x801b0f68u;
                state.fpscr = 4;
                state.hid0 = 0xc200u;
                state.fpr.fill({UINT64_MAX, UINT64_MAX});
                ppc_write_u32(state, 0x803495e4u, 0x80003000u);
                ppc_write_u32(state, 0x80003020u, installed ? 0x801b1c84u : 0x801b152cu);
                ppc_write_u64(state, 0x80349600u, UINT64_C(0x1111111122222222));
                constexpr uint32_t reset = 0x802dfe80u;
                constexpr uint32_t other = 0x80004000u;
                ppc_write_u32(state, other, 0x80001000u);
                ppc_write_u32(state, other + 4, 1);
                ppc_write_u64(state, other + 8, 0);
                ppc_write_u32(state, 0x80349648u, existingReset ? other : 0);
                ppc_write_u32(state, 0x8034964cu, existingReset ? other : 0);
                for (unsigned repeat = 0; repeat < 2; ++repeat) {
                    const auto before = state;
                    auto expected = state;
                    const bool initialize = !installed && repeat == 0;
                    if (initialize) {
                        ppc_write_u32(expected, 0x80003020u, 0x801b1c84u);
                        ppc_write_u64(expected, 0x80349600u, 0);
                        ppc_write_u32(expected, 0x80349648u, existingReset ? other : reset);
                        ppc_write_u32(expected, 0x8034964cu, reset);
                        ppc_write_u32(expected, reset + 8, 0);
                        ppc_write_u32(expected, reset + 12, existingReset ? other : 0);
                        if (existingReset) ppc_write_u32(expected, other + 8, reset);
                    }
                    std::vector<uint32_t> calls;
                    state.callGuest = [&](uint32_t address, PpcState& guest) {
                        Require(calls.size() < 3, "Alarm initialization exceeded dependency budget.");
                        calls.push_back(address);
                        if (address == 0x801b5550u) {
                            Require(guest.gpr[3] == reset, "Alarm registered the wrong reset node.");
                        } else {
                            Require(guest.gpr[3] == 8, "Alarm accessed the wrong exception handler.");
                        }
                        RunHardwareDependency(address, guest);
                    };
                    ppc_801B1618(state);
                    const std::vector<uint32_t> expectedCalls = initialize ?
                        std::vector<uint32_t>{0x801b147cu, 0x801b1460u, 0x801b5550u} :
                        std::vector<uint32_t>{0x801b147cu};
                    Require(calls == expectedCalls && state.pc == before.lr && state.lr == before.lr &&
                                state.externalInterruptsEnabled == interrupts &&
                                state.fpr == before.fpr && state.fpscr == before.fpscr &&
                                state.hid0 == before.hid0 && state.xer == before.xer &&
                                state.ctr == before.ctr,
                            "Original alarm initialization failed call order, re-entry or CPU guards.");
                    for (unsigned reg = 0; reg < 32; ++reg) {
                        if (reg == 0 || (reg >= 3 && reg <= 5)) continue;
                        Require(state.gpr[reg] == before.gpr[reg],
                                "Alarm initialization damaged preserved integer registers.");
                    }
                    const size_t frame = ppc_memory_offset(state, before.gpr[1] - 8, 16);
                    for (size_t i = 0; i < state.memory.size(); ++i) {
                        if (i >= frame && i < frame + 16) continue;
                        Require(state.memory[i] == expected.memory[i],
                                "Alarm initialization changed unexpected RAM or duplicated registration.");
                    }
                }
            }
        }
    }
    std::cout << "Original OSInitAlarm clears the queue, installs decrementer handler and "
                 "registers reset info; existing handler/re-entry preserve queues and list. "
                 "No alarm scheduling or decrementer delivery implemented.\n";
}

void TestOriginalModuleInitialization(const DolImage& image) {
    for (bool interrupts : {false, true}) {
        for (uint32_t pattern : {0u, 0x12345678u, UINT32_MAX}) {
            auto state = image.CreateGameCubeState();
            ppc_800032B0(state);
            state.externalInterruptsEnabled = interrupts;
            state.lr = 0x801b0f6cu;
            state.ctr = 0x12345678u;
            state.cr = 0x87654321u;
            state.xer = 0xe0000000u;
            state.fpscr = 4;
            state.msr = 0x2030u;
            state.hid0 = 0xc200u;
            state.hid2 = 0xa0000000u;
            state.l2cr = 0x80000000u;
            state.fpr.fill({UINT64_MAX, UINT64_MAX});
            state.gqr.fill(0x12345678u);
            state.performanceMonitor.fill(0x87654321u);
            for (uint32_t address : {0x800030c8u, 0x800030ccu, 0x800030d0u}) {
                ppc_write_u32(state, address, pattern);
            }
            // An independent linked-module payload must not be cleared or traversed.
            ppc_write_u32(state, 0x80004000u, 0x11223344u);
            state.callGuest = [](uint32_t, PpcState&) {
                throw std::runtime_error("Module initialization unexpectedly called another function.");
            };
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                const auto before = state;
                auto expected = state;
                ppc_write_u32(expected, 0x800030c8u, 0);
                ppc_write_u32(expected, 0x800030ccu, 0);
                ppc_write_u32(expected, 0x800030d0u, 0);
                ppc_801B4C90(state);
                Require(state.memory == expected.memory && state.pc == before.lr &&
                            state.lr == before.lr && state.ctr == before.ctr &&
                            state.cr == before.cr && state.xer == before.xer &&
                            state.fpscr == before.fpscr && state.fpr == before.fpr &&
                            state.gqr == before.gqr && state.msr == before.msr &&
                            state.hid0 == before.hid0 && state.hid2 == before.hid2 &&
                            state.l2cr == before.l2cr &&
                            state.performanceMonitor == before.performanceMonitor &&
                            state.externalInterruptsEnabled == interrupts &&
                            state.gpr[0] == 0 && state.gpr[4] == 0x80000000u,
                        "Original module initializer failed exact low-memory reset or CPU guards.");
                for (unsigned reg = 0; reg < 32; ++reg) {
                    if (reg == 0 || reg == 4) continue;
                    Require(state.gpr[reg] == before.gpr[reg],
                            "Module initializer damaged an unrelated integer register.");
                }
            }
        }
    }
    std::cout << "Original __OSModuleInit resets exactly 0x800030c8/cc/d0; "
                 "poisoned/zero state, repeated calls, both interrupt states and CPU/RAM guards passed. "
                 "No REL link/unlink or native-binding lifecycle implied.\n";
}

void ExpectInterruptTableInitialization(PpcState& expected, uint32_t stack, uint32_t lr) {
    ppc_write_u32(expected, stack + 4, lr);
    ppc_write_u32(expected, stack - 0x10u, stack);
    ppc_write_u32(expected, stack - 4, expected.gpr[31]);
    ppc_write_u32(expected, stack - 0x20u, stack - 0x10u);
    ppc_write_u32(expected, stack - 0xcu, 0x801b3aecu);
    ppc_write_u32(expected, stack - 0x14u, 0x80000000u);
    ppc_write_u32(expected, 0x80349620u, 0x80003040u);
    for (uint32_t i = 0; i < 0x80; ++i) ppc_write_u8(expected, 0x80003040u + i, 0);
    ppc_write_u32(expected, 0x800000c4u, 0);
    ppc_write_u32(expected, 0x800000c8u, 0);
}

void RunInterruptInitializationToHardware(PpcState& state) {
    state.callGuest = [](uint32_t address, PpcState& guest) {
        switch (address) {
        case 0x800054e4u: ppc_800054E4(guest); break;
        case 0x80005514u: ppc_80005514(guest); break;
        default:
            throw std::runtime_error("Unexpected interrupt-init dependency: " +
                                     std::to_string(address));
        }
        Require(guest.pc == guest.lr, "Interrupt-init memory dependency did not return.");
    };
    bool rejected = false;
    try {
        ppc_801B3AC0(state);
    } catch (const std::out_of_range& error) {
        rejected = std::string(error.what()).find("not mapped to GameCube RAM") != std::string::npos;
    }
    Require(rejected && state.gpr[0] == 0xf0u && state.gpr[4] == 0xcc003000u &&
                state.gpr[3] == 0xffffffe0u,
            "Original interrupt initialization did not stop at the PI mask hardware write.");
}

void TestOriginalInterruptTableInitialization(const DolImage& image) {
    for (bool interrupts : {false, true}) {
        auto state = image.CreateGameCubeState();
        ppc_800032B0(state);
        state.externalInterruptsEnabled = interrupts;
        state.lr = 0x801b0f70u;
        state.fpscr = 4;
        state.msr = 0x2030u;
        state.hid0 = 0xc200u;
        state.hid2 = 0xa0000000u;
        state.l2cr = 0x80000000u;
        state.fpr.fill({UINT64_MAX, UINT64_MAX});
        state.gqr.fill(0x12345678u);
        state.performanceMonitor.fill(0x87654321u);
        std::fill(state.memory.begin() + 0x3040, state.memory.begin() + 0x30c0, 0xa5);
        ppc_write_u32(state, 0x800000c4u, UINT32_MAX);
        ppc_write_u32(state, 0x800000c8u, 0x12345678u);
        const auto before = state;
        auto expected = state;
        const uint32_t stack = state.gpr[1];
        ExpectInterruptTableInitialization(expected, stack, state.lr);
        RunInterruptInitializationToHardware(state);
        Require(state.memory == expected.memory && state.gpr[1] == stack - 0x10u &&
                    state.gpr[31] == 0x80000000u && state.fpr == before.fpr &&
                    state.fpscr == before.fpscr && state.gqr == before.gqr &&
                    state.msr == before.msr && state.hid0 == before.hid0 &&
                    state.hid2 == before.hid2 && state.l2cr == before.l2cr &&
                    state.performanceMonitor == before.performanceMonitor &&
                    state.externalInterruptsEnabled == interrupts,
                "Interrupt-init RAM prefix changed unexpected memory or CPU control state.");
        for (unsigned reg = 14; reg < 31; ++reg) {
            Require(state.gpr[reg] == before.gpr[reg],
                    "Interrupt-init prefix damaged a callee-saved register.");
        }
        const auto blocked = state.memory;
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            bool rejected = false;
            try {
                ppc_801B3B08(state);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            Require(rejected && state.memory == blocked,
                    "Blocked PI mask write changed RAM or silently succeeded on retry.");
        }
    }
    std::cout << "Original __OSInterruptInit clears its 32-slot handler table and software "
                 "masks, then explicitly rejects the unsupported PI mask write at 0x801b3b08 "
                 "(0xcc003004). The saved frame remains active; initialization has not returned.\n";
}

void RunInterruptPiSetupToDevice(PpcState& state) {
    const uint32_t stack = state.gpr[1];
    auto expected = state;
    ExpectInterruptTableInitialization(expected, stack, state.lr);
    ppc_write_u32(expected, stack - 0xcu, 0x801b3b10u);
    ppc_write_u32(expected, stack - 0x30u, stack - 0x10u);
    ppc_write_u32(expected, stack - 0x18u, state.gpr[30]);
    ppc_write_u32(expected, stack - 0x1cu, state.gpr[29]);
    ppc_write_u32(expected, 0x800000c4u, 0xffffffe0u);
    unsigned calls = 0;
    state.callGuest = [&](uint32_t address, PpcState& guest) {
        Require(++calls < 10, "PI startup exceeded its dependency budget.");
        switch (address) {
        case 0x800054e4u: ppc_800054E4(guest); break;
        case 0x80005514u: ppc_80005514(guest); break;
        case 0x801b3e0cu:
            Require(ppc_read_u32(guest, 0xcc003004u) == 0xf0u,
                    "Original interrupt initialization did not write the PI startup mask.");
            ppc_801B3E0C(guest);
            break;
        case 0x801b3a44u: ppc_801B3A44(guest); break;
        case 0x801b3b34u: ppc_801B3B34(guest); break;
        default:
            throw std::runtime_error("Unexpected PI startup dependency: " +
                                     std::to_string(address));
        }
    };
    bool rejected = false;
    try {
        ppc_801B3AC0(state);
    } catch (const std::out_of_range& error) {
        rejected = std::string(error.what()).find("not mapped to GameCube RAM") != std::string::npos;
    }
    const bool miEnabled = state.miInterruptMask.has_value();
    const bool dspEnabled = state.dspControl.has_value();
    Require(rejected && calls == (dspEnabled ? 7u : miEnabled ? 6u : 5u) &&
                state.piInterruptMask == 0xf0u &&
                ppc_read_u32(state, 0x800000c4u) == 0xffffffe0u &&
                ppc_read_u32(state, 0x800000c8u) == 0 &&
                (!dspEnabled || state.dspControl == (*expected.dspControl & ~0x0150u)) &&
                (dspEnabled ? state.miInterruptMask == 0 &&
                    state.gpr[4] == 0xcc000000u && state.gpr[0] == 0x00800000u :
                 miEnabled ? state.miInterruptMask == 0 &&
                    state.gpr[5] == 0xcc00500au && state.gpr[0] == 0x04000000u :
                    state.gpr[4] == 0xcc004000u && state.gpr[0] == 0) &&
                !state.externalInterruptsEnabled &&
                state.gpr[1] == stack - 0x30u &&
                state.memory == expected.memory &&
                state.fpr == expected.fpr && state.fpscr == expected.fpscr &&
                state.gqr == expected.gqr && state.msr == expected.msr &&
                state.hid0 == expected.hid0 && state.hid2 == expected.hid2 &&
                state.l2cr == expected.l2cr &&
                state.performanceMonitor == expected.performanceMonitor,
            "PI/MI-backed startup did not reach the next unsupported device access with original state.");
    state.callGuest = {};
}

void TestOriginalInterruptPiSetup(const DolImage& image) {
    for (bool interrupts : {false, true}) {
        auto state = image.CreateGameCubeState();
        ppc_800032B0(state);
        state.lr = 0x801b0f70u;
        state.externalInterruptsEnabled = interrupts;
        state.piInterruptMask = 0;
        const auto before = state;
        RunInterruptPiSetupToDevice(state);
        for (uint16_t mask : {uint16_t{0}, uint16_t{0x1f}, uint16_t{0xffff}}) {
            state = before;
            state.miInterruptMask = mask;
            RunInterruptPiSetupToDevice(state);
        }
        for (uint16_t control : {uint16_t{0}, uint16_t{4}, uint16_t{0x154}, uint16_t{0x7fc}}) {
            state = before;
            state.miInterruptMask = 0x1fu;
            state.dspControl = control;
            RunInterruptPiSetupToDevice(state);
        }
    }
    std::cout << "Opt-in PI mask register accepts original 0xf0 startup write; original "
                 "__OSMaskInterrupts/SetInterruptMask then reject MI write at 0x801b3be0 "
                 "(0xcc00401c). No device interrupts are delivered.\n";
    std::cout << "With opt-in MI mask state, original masking clears MI enables and reaches "
                 "unsupported DSP control read at 0x801b3bfc (0xcc00500a). "
                 "Interrupt initialization has not returned.\n";
    std::cout << "With opt-in DSP control, original interrupt masking disables DSP enables "
                 "without clearing pending status; next unsupported access is AI control "
                 "at 0x801b3c4c (0xcc006c00).\n";
}

void TestOriginalInterruptHandlerAccessors(const DolImage& image) {
    for (bool interrupts : {false, true}) {
        auto state = image.CreateGameCubeState();
        ppc_800032B0(state);
        state.externalInterruptsEnabled = interrupts;
        state.lr = 0x80001000u;
        state.cr = 0x12345678u;
        state.xer = 0xe0000000u;
        state.fpscr = 4;
        for (uint32_t table : {0x80003040u, 0xc0003040u}) {
            ppc_write_u32(state, 0x80349620u, table);
            for (unsigned test = 0; test < 34; ++test) {
                const uint32_t index = test < 32 ? test : test == 32 ? 0xffffu : 0xfffeu;
                const uint32_t offset = test < 32 ? test * 4 : test == 32 ? 0xfffffffcu : 0xfffffff8u;
                const uint32_t old = 0x80001000u + test * 4;
                const uint32_t handler = test == 0 ? 0 : 0x801b0000u + test * 4;
                ppc_write_u32(state, table + offset, old);
                const auto before = state;
                auto expected = state;
                ppc_write_u32(expected, table + offset, handler);
                state.gpr[3] = 0xabcd0000u | index;
                state.gpr[4] = handler;
                ppc_801B3A90(state);
                Require(state.gpr[3] == old && state.memory == expected.memory &&
                            state.pc == state.lr,
                        "Original interrupt setter failed old-value return or exact table write.");
                state.gpr[3] = 0x12340000u | index;
                ppc_801B3AAC(state);
                Require(state.gpr[3] == handler && state.memory == expected.memory &&
                            state.pc == state.lr && state.cr == before.cr &&
                            state.xer == before.xer && state.fpscr == before.fpscr &&
                            state.externalInterruptsEnabled == interrupts &&
                            state.gpr[1] == before.gpr[1],
                        "Original interrupt getter failed selector truncation or state guards.");
                for (unsigned reg = 0; reg < 32; ++reg) {
                    if (reg == 0 || (reg >= 3 && reg <= 5)) continue;
                    Require(state.gpr[reg] == before.gpr[reg],
                            "Interrupt accessor damaged an unrelated integer register.");
                }
            }
        }
    }
    std::cout << "Original interrupt handler getter/setter passed all 32 slots, RAM aliases, "
                 "null handlers, signed low-halfword selectors and previous-handler returns.\n";
}

void TestEntryThroughHardwareAndMemory(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    Require(ppc_read_u64(state, 0x803495d0u) == 0 &&
                ppc_read_u64(state, 0x803495d8u) == 0,
            "Loader must provide zero constants before original FPR initialization.");
    state.msr = 0x30u;
    state.externalInterruptsEnabled = false;
    state.gqr.fill(UINT32_MAX);
    state.fpr.fill({UINT64_MAX, UINT64_MAX});
    state.fpscr = UINT32_MAX;
    std::vector<std::pair<uint32_t, uint32_t>> bss;
    for (uint32_t table = 0x800056a0u; ppc_read_u32(state, table + 4) != 0; table += 8) {
        Require(bss.size() < 3, "Unexpected startup BSS table size.");
        const auto address = ppc_read_u32(state, table);
        const auto size = ppc_read_u32(state, table + 4);
        const size_t offset = ppc_memory_offset(state, address, size);
        std::fill_n(state.memory.begin() + offset, size, 0xa5);
        bss.push_back({address, size});
    }
    Require(bss.size() == 3, "Entry fixture requires three verified BSS ranges.");
    // Hardware reads these BSS constants before the game's BSS clear.
    ppc_write_u64(state, 0x803495d0u, 0);
    ppc_write_u64(state, 0x803495d8u, 0);
    std::vector<uint32_t> topCalls;
    unsigned calls = 0;
    state.callGuest = [&](uint32_t address, PpcState& guest) {
        Require(++calls < 100, "Entry fixture exceeded its dependency budget.");
        switch (address) {
        case 0x800032b0u:
            topCalls.push_back(address);
            ppc_800032B0(guest);
            break;
        case 0x80003400u:
            topCalls.push_back(address);
            ppc_80003400(guest);
            Require(ppc_read_u32(guest, 0x80317a94u) == 0x801b287cu,
                    "Entry hardware call did not register the original DMA error handler.");
            break;
        case 0x80003340u:
            topCalls.push_back(address);
            ppc_80003340(guest);
            break;
        case 0x801d3348u:
            topCalls.push_back(address);
            ppc_801D3348(guest);
            break;
        case 0x800055ccu: ppc_800055CC(guest); break;
        case 0x800054e4u: ppc_800054E4(guest); break;
        case 0x80005514u: ppc_80005514(guest); break;
        default: RunHardwareDependency(address, guest); break;
        }
        Require(guest.pc == guest.lr, "Entry dependency did not return to its original caller.");
    };
    const auto before = state.memory;
    ppc_80003154(state);
    Require(topCalls == std::vector<uint32_t>{0x800032b0u, 0x80003400u, 0x80003340u,
                                             0x801d3348u} &&
                state.pc == 0x80003264u && state.gpr[1] == 0x8035cf98u &&
                state.gpr[2] == 0x80351e40u && state.gpr[13] == 0x8034e9c0u &&
                state.gpr[14] == 0 && state.gpr[15] == 0 &&
                ppc_read_u32(state, state.gpr[1]) == UINT32_MAX &&
                ppc_read_u32(state, state.gpr[1] + 4) == 0x80003170u,
            "Original entry did not reach OSInit with the expected calls, stack and SDA registers.");
    Require(ppc_read_u32(state, 0x80349900u) == 0x80000040u &&
                ppc_read_u32(state, 0x80349904u) == 1u &&
                ppc_read_u32(state, 0x80000048u) == 0x001d33b8u,
            "Original DBInit did not install its interface, verbose flag and exception destination.");
    std::ostringstream controls;
    controls << std::hex << "Original entry control state: HID0=" << state.hid0
             << " HID2=" << state.hid2 << " L2CR=" << state.l2cr
             << " MSR=" << ppc_read_msr(state) << " FPSCR=" << state.fpscr;
    Require(state.hid0 == 0xc000u && state.hid2 == 0xa0000000u &&
                state.l2cr == 0x80000000u && ppc_read_msr(state) == 0x2030u &&
                !state.externalInterruptsEnabled && state.fpscr == 0,
            controls.str().c_str());
    Require(std::all_of(state.gqr.begin(), state.gqr.end(),
                            [](uint32_t v) { return v == 0; }),
            "Original entry changed GQR state after initialization.");
    Require(
                std::all_of(state.fpr.begin(), state.fpr.end(),
                            [](const PpcFpr& v) { return v == PpcFpr{0, 0}; }),
            "Original entry changed FPR state after initialization.");
    for (const auto& range : bss) {
        const auto offset = ppc_memory_offset(state, range.first, range.second);
        for (size_t i = offset; i < offset + range.second; ++i) {
            if (i >= 0x349900u && i < 0x349908u) continue;
            Require(state.memory[i] == 0,
                    "Entry BSS is nonzero outside the original DBInit globals.");
        }
    }
    const auto frame = ppc_memory_offset(state, 0x8035cef8u, 0xb0);
    for (size_t i = 0; i < before.size(); ++i) {
        const bool cleared = std::any_of(bss.begin(), bss.end(), [&](const auto& range) {
            const auto offset = ppc_memory_offset(state, range.first, range.second);
            return i >= offset && i < offset + range.second;
        });
        if (cleared || (i >= frame && i < frame + 0xb0) ||
            (i >= 0x44u && i < 0x4cu)) continue;
        Require(state.memory[i] == before[i], "Entry wrote outside BSS/stack/boot-word guards.");
    }
    std::cout << "Original entry 0x80003154 -> complete hardware -> original ROM/BSS "
                 "initialization -> argument setup -> original DBInit reaches OSInit "
                 "at 0x80003264. OSInit is not executed; loader IR/DR precondition, no game boot.\n";
    const uint32_t stack = state.gpr[1];
    const uint32_t r29 = state.gpr[29], r30 = state.gpr[30], r31 = state.gpr[31];
    constexpr uint64_t ticks = UINT64_C(0x12345678ffffffff);
    constexpr uint64_t offset = 2;
    ppc_write_u64(state, 0x800030d8u, offset);
    unsigned reads = 0;
    std::vector<uint32_t> setupCalls;
    const auto fpr = state.fpr;
    state.performanceMonitor.fill(UINT32_MAX);
    ppc_write_u32(state, 0x800030c8u, 0xdeadbeefu);
    ppc_write_u32(state, 0x800030ccu, 0x12345678u);
    ppc_write_u32(state, 0x800030d0u, UINT32_MAX);
    calls = 0;
    state.callGuest = [&](uint32_t address, PpcState& guest) {
        Require(++calls < 150, "OSInit fixture exceeded its dependency budget.");
        if (address >= 0x801b0a6cu && address <= 0x801b0a94u) {
            Require(!guest.externalInterruptsEnabled && guest.gpr[3] == 0,
                    "OSInit did not reset performance monitoring under its interrupt mask.");
            setupCalls.push_back(address);
        } else if (address == 0x801b0afcu || address == 0x801b0b24u) {
            Require(!guest.externalInterruptsEnabled, "OSInit setup lost its interrupt mask.");
            setupCalls.push_back(address);
        }
        RunHardwareDependency(address, guest);
    };
    state.readTimeBase = [&] {
        Require(!state.externalInterruptsEnabled,
                "OSInit system-time sampling did not mask interrupts.");
        Require(++reads <= 3, "OSInit system-time sampling did not converge.");
        return ticks;
    };
    state.lr = 0x80003268u;
    auto expectedExceptions = state;
    ExpectExceptionInitialization(expectedExceptions, false, 0,
                                   ppc_read_u32(state, 0x80000044u));
    ExpectSystemCallInstallation(expectedExceptions);
    ppc_write_u32(expectedExceptions, 0x80003020u, 0x801b1c84u);
    ppc_write_u64(expectedExceptions, 0x80349600u, 0);
    ppc_write_u32(expectedExceptions, 0x80349648u, 0x802dfe80u);
    ppc_write_u32(expectedExceptions, 0x8034964cu, 0x802dfe80u);
    ppc_write_u64(expectedExceptions, 0x802dfe88u, 0);
    ppc_write_u32(expectedExceptions, 0x800030c8u, 0);
    ppc_write_u32(expectedExceptions, 0x800030ccu, 0);
    ppc_write_u32(expectedExceptions, 0x800030d0u, 0);
    ppc_801B0DE0(state);
    Require(state.pc == 0x801b0f6cu && reads == 3 &&
                setupCalls == std::vector<uint32_t>{0x801b0a6cu, 0x801b0a74u,
                    0x801b0a7cu, 0x801b0a84u, 0x801b0a8cu, 0x801b0a94u,
                    0x801b0afcu, 0x801b0b24u} &&
                state.hid0 == 0xc200u && state.fpscr == 4 && state.fpr == fpr &&
                std::all_of(state.performanceMonitor.begin(), state.performanceMonitor.end(),
                            [](uint32_t value) { return value == 0; }) &&
                state.gpr[1] == stack - 0x18u &&
                ppc_read_u32(state, 0x803495e0u) == 1 &&
                ppc_read_u64(state, 0x803495f8u) == ticks + offset &&
                ppc_read_u32(state, 0x803495c0u) == 0x80000000u &&
                ppc_read_u32(state, 0x803495c4u) == 0 &&
                ppc_read_u32(state, 0x803496ecu) == 1 &&
                ppc_read_u32(state, 0x80348b90u) == 0x8035efa0u &&
                ppc_read_u32(state, 0x80349618u) == 0x81700000u &&
                !state.externalInterruptsEnabled,
            "Original OSInit prefix failed time, interrupts, monitor reset or SPD/NI setup.");
    const auto prefixFrame = ppc_memory_offset(state, stack - 0x38u, 0x40);
    for (size_t i = 0; i < expectedExceptions.memory.size(); ++i) {
        if ((i >= prefixFrame - 0x88u && i < prefixFrame + 0x40) ||
            (i >= 0x3495c0u && i < 0x3495c8u) ||
            (i >= 0x3496ecu && i < 0x3496f0u) ||
            (i >= 0x348b90u && i < 0x348b94u) ||
            (i >= 0x349618u && i < 0x34961cu) ||
            (i >= 0x3495e0u && i < 0x3495e4u) ||
            (i >= 0x3495f8u && i < 0x349600u)) continue;
        Require(state.memory[i] == expectedExceptions.memory[i],
                "OSInit prefix wrote outside stack, initialization flag and start time.");
    }
    const auto firstTime = state;
    const auto started = state.memory;
    // Finish only the saved frame for the re-entry test; the first startup is incomplete.
    ppc_801B119C(state);
    Require(state.pc == 0x80003268u && state.gpr[1] == stack &&
                state.gpr[29] == r29 && state.gpr[30] == r30 && state.gpr[31] == r31,
            "Original OSInit epilogue did not restore its saved frame.");
    state.externalInterruptsEnabled = true;
    setupCalls.clear();
    state.performanceMonitor.fill(0x12345678u);
    state.hid0 = 0xc000u;
    state.fpscr = 3;
    ppc_801B0DE0(state);
    Require(state.pc == 0x801b119cu && reads == 3 && state.externalInterruptsEnabled &&
                setupCalls.empty() && state.hid0 == 0xc000u && state.fpscr == 3 &&
                std::all_of(state.performanceMonitor.begin(), state.performanceMonitor.end(),
                            [](uint32_t value) { return value == 0x12345678u; }),
            "Original OSInit re-entry did not skip initialization.");
    ppc_801B119C(state);
    Require(state.pc == state.lr && state.gpr[1] == stack &&
                state.gpr[29] == r29 && state.gpr[30] == r30 && state.gpr[31] == r31,
            "Original OSInit re-entry damaged its return ABI.");
    const auto osFrame = ppc_memory_offset(state, stack - 0x18u, 0x20);
    for (size_t i = 0; i < started.size(); ++i) {
        if (i >= osFrame && i < osFrame + 0x20) continue;
        Require(state.memory[i] == started[i],
                "OSInit re-entry changed memory outside its stack frame.");
    }
    std::cout << "Original OSInit prefix sets initialization flag/start time, masks "
                 "interrupts, resets MMCR0/1 plus PMC1..4, sets HID0 SPD and FPSCR NI; "
                 "initializes boot info, arenas, exception and system-call vectors; "
                 "initializes the alarm queue/handler/reset node; "
                 "resets module metadata, stops at 0x801b0f6c before __OSInterruptInit. "
                 "Re-entry skip/epilogue tested separately; first OSInit does not return.\n";
    state = firstTime;
    state.lr = 0x801b0f70u;
    auto expectedInterrupts = state;
    ExpectInterruptTableInitialization(expectedInterrupts, state.gpr[1], state.lr);
    RunInterruptInitializationToHardware(state);
    Require(state.memory == expectedInterrupts.memory && !state.externalInterruptsEnabled,
            "Entry-derived interrupt prefix changed RAM outside its exact initialization writes.");
    std::cout << "Entry-derived state continues into original interrupt table/mask reset, "
                 "then rejects PI MMIO at 0x801b3b08; OSInit has not returned.\n";
    state = firstTime;
    state.lr = 0x801b0f70u;
    state.piInterruptMask = 0;
    RunInterruptPiSetupToDevice(state);
    std::cout << "With opt-in PI mask state, entry-derived interrupt initialization reaches "
                 "the unsupported MI mask write at 0x801b3be0; OSInit remains incomplete.\n";
    state = firstTime;
    state.lr = 0x801b0f70u;
    state.piInterruptMask = 0;
    state.miInterruptMask = 0x1fu;
    RunInterruptPiSetupToDevice(state);
    std::cout << "With opt-in PI/MI state, entry-derived setup reaches unsupported DSP "
                 "control read at 0x801b3bfc; OSInit remains incomplete.\n";
    state = firstTime;
    state.lr = 0x801b0f70u;
    state.piInterruptMask = 0;
    state.miInterruptMask = 0x1fu;
    state.dspControl = 0x7fcu;
    RunInterruptPiSetupToDevice(state);
    std::cout << "With opt-in PI/MI/DSP state, entry-derived setup reaches unsupported AI "
                 "control read at 0x801b3c4c; OSInit remains incomplete.\n";
}

void TestOriginalArenaAccessors(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.lr = 0x80001000u;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 4;
    for (bool interrupts : {false, true}) {
        state.externalInterruptsEnabled = interrupts;
        for (uint32_t value : {0u, 0x8035cfa1u, 0x81700000u, UINT32_MAX}) {
            for (bool high : {false, true}) {
                const uint32_t slot = high ? 0x80349618u : 0x80348b90u;
                auto expected = state;
                ppc_write_u32(expected, slot, value);
                state.gpr[3] = value;
                if (high) ppc_801B20E4(state);
                else ppc_801B20EC(state);
                Require(state.memory == expected.memory && state.pc == state.lr &&
                            state.gpr[3] == value,
                        "Original arena setter changed more than its SDA slot.");
                state.gpr[3] = ~value;
                if (high) ppc_801B20D4(state);
                else ppc_801B20DC(state);
                Require(state.gpr[3] == value && state.memory == expected.memory &&
                            state.pc == state.lr && state.cr == 0x12345678u &&
                            state.xer == 0xe0000000u && state.fpscr == 4 &&
                            state.gpr[1] == 0x8035cfa0u &&
                            state.externalInterruptsEnabled == interrupts,
                        "Original arena getter changed memory, flags, stack or interrupt state.");
                Require(state.fpr == expected.fpr && state.gqr == expected.gqr &&
                            state.msr == expected.msr && state.hid0 == expected.hid0 &&
                            state.hid2 == expected.hid2 && state.l2cr == expected.l2cr &&
                            state.performanceMonitor == expected.performanceMonitor &&
                            state.ctr == expected.ctr && state.lr == expected.lr,
                        "Original arena accessors changed unrelated CPU state.");
                for (unsigned reg = 0; reg < 32; ++reg) {
                    if (reg == 3) continue;
                    Require(state.gpr[reg] == expected.gpr[reg],
                            "Original arena accessors changed an unrelated integer register.");
                }
            }
        }
    }
    std::cout << "Original arena getters/setters preserve exact bounds, including zero/"
                 "unaligned values; no allocator or host pointer conversion implied.\n";
}

void TestOriginalBootInfoAndArena(const DolImage& image) {
    for (unsigned source = 0; source < 3; ++source) {
        for (uint32_t debug : {0u, 1u, 2u, 0x100u, UINT32_MAX}) {
            for (bool customLow : {false, true}) {
                for (bool customHigh : {false, true}) {
                    if (source == 0 && customHigh) continue; // Nonzero high selects fallback boot info.
                    if (source == 2 && !customHigh) continue;
                    auto state = image.CreateGameCubeState();
                    ppc_800032B0(state);
                    state.externalInterruptsEnabled = false;
                    state.hid0 = 0xc200u;
                    state.fpscr = 4;
                    state.fpr.fill({UINT64_MAX, UINT64_MAX});
                    state.gqr.fill(0x12345678u);
                    state.performanceMonitor.fill(0x87654321u);
                    constexpr uint32_t bi2 = 0x80004000u;
                    constexpr uint32_t pad = 0x123456abu;
                    const uint32_t low = customLow ? 0x80400001u : 0;
                    const uint32_t high = customHigh ? 0x81600001u : 0;
                    ppc_write_u32(state, 0x800000f4u, source == 1 ? bi2 : 0);
                    ppc_write_u32(state, 0x80000030u, low);
                    ppc_write_u32(state, 0x80000034u, high);
                    ppc_write_u32(state, bi2 + 0xc, debug);
                    ppc_write_u32(state, bi2 + 0x24, pad);
                    ppc_write_u8(state, 0x800030e8u,
                        source == 1 ? 0x55u : static_cast<uint8_t>(debug));
                    ppc_write_u8(state, 0x800030e9u,
                        source == 1 ? 0x66u : static_cast<uint8_t>(pad));
                    for (uint32_t slot : {0x803495c0u, 0x803495c4u, 0x803495c8u,
                                          0x803497e0u, 0x803496ecu, 0x80348b90u,
                                          0x80349618u}) {
                        ppc_write_u32(state, slot, 0xdeadbeefu);
                    }
                    const auto before = state;
                    auto expected = state;
                    ppc_write_u32(expected, 0x803495c0u, 0x80000000u);
                    ppc_write_u32(expected, 0x803495c4u,
                        source == 1 ? bi2 + 0xc : source == 2 ? 0x803495c8u : 0);
                    if (source == 1) {
                        ppc_write_u32(expected, 0x803497e0u, pad);
                        ppc_write_u8(expected, 0x800030e8u, static_cast<uint8_t>(debug));
                        ppc_write_u8(expected, 0x800030e9u, static_cast<uint8_t>(pad));
                    } else if (source == 2) {
                        ppc_write_u32(expected, 0x803495c8u, debug & 0xffu);
                        ppc_write_u32(expected, 0x803497e0u, pad & 0xffu);
                    }
                    ppc_write_u32(expected, 0x803496ecu, 1);
                    const uint32_t effectiveDebug = source == 2 ? debug & 0xffu : debug;
                    const bool stackArena = !customLow && source != 0 && effectiveDebug < 2;
                    const uint32_t arenaLow = customLow ? low :
                        stackArena ? 0x8035cfa0u : 0x8035efa0u;
                    const uint32_t arenaHigh = customHigh ? high : 0x81700000u;
                    ppc_write_u32(expected, 0x80348b90u, arenaLow);
                    ppc_write_u32(expected, 0x80349618u, arenaHigh);
                    std::vector<std::pair<uint32_t, uint32_t>> setters;
                    state.callGuest = [&](uint32_t address, PpcState& guest) {
                        Require(setters.size() < 3 && !guest.externalInterruptsEnabled,
                                "Boot-info fixture exceeded setter budget or enabled interrupts.");
                        setters.push_back({address, guest.gpr[3]});
                        RunHardwareDependency(address, guest);
                    };
                    ppc_801B0E64(state);
                    std::vector<std::pair<uint32_t, uint32_t>> expectedSetters{
                        {0x801b20ecu, customLow ? low : 0x8035efa0u}};
                    if (stackArena) expectedSetters.push_back({0x801b20ecu, arenaLow});
                    expectedSetters.push_back({0x801b20e4u, arenaHigh});
                    Require(state.pc == 0x801b0f5cu && !state.externalInterruptsEnabled &&
                                setters == expectedSetters &&
                                state.memory == expected.memory,
                            "Original boot-info/arena branches, exact writes or setter order differ.");
                    Require(state.fpr == before.fpr && state.gqr == before.gqr &&
                                state.fpscr == before.fpscr && state.hid0 == before.hid0 &&
                                state.msr == before.msr && state.hid2 == before.hid2 &&
                                state.l2cr == before.l2cr && state.xer == before.xer &&
                                (state.cr & 0x0fffffffu) == (before.cr & 0x0fffffffu) &&
                                state.lr == 0x801b0f5cu &&
                                state.ctr == before.ctr &&
                                state.performanceMonitor == before.performanceMonitor,
                            "Boot-info setup damaged unrelated CPU state.");
                    for (unsigned reg = 0; reg < 32; ++reg) {
                        if (reg == 0 || reg == 3 || reg == 4) continue;
                        Require(state.gpr[reg] == before.gpr[reg],
                                "Boot-info setup damaged its stack, SDA or preserved registers.");
                    }
                }
            }
        }
    }
    std::cout << "Original boot-info/arena setup passed BI2, low-memory fallback and absent "
                 "metadata; debug thresholds, byte truncation, custom/default bounds, "
                 "setter order and exact RAM guards. Stops before OSExceptionInit.\n";
}

void TestMemoryFunctions(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    BindMemoryFunctions(state);
    const uint32_t stack = state.gpr[1];
    constexpr uint32_t base = 0x80001000u;
    for (uint32_t alignment = 0; alignment < 4; ++alignment) {
        for (uint32_t count : {0u, 1u, 3u, 4u, 7u, 31u, 32u, 33u, 63u, 64u, 65u, 257u}) {
            for (uint32_t fill : {0u, 0xa5u, 0x123456ffu}) {
                std::vector<uint8_t> expected(512, 0xcc);
                for (uint32_t i = 0; i < expected.size(); ++i) {
                    ppc_write_u8(state, base + i, expected[i]);
                }
                const uint32_t start = 8u + alignment;
                std::fill_n(expected.begin() + start, count, static_cast<uint8_t>(fill));
                state.gpr[3] = base + start;
                state.gpr[4] = fill;
                state.gpr[5] = count;
                state.gpr[31] = 0x98765432u;
                state.lr = 0x80001000u;
                ppc_800054E4(state);
                for (uint32_t i = 0; i < expected.size(); ++i) {
                    Require(ppc_read_u8(state, base + i) == expected[i],
                            "Original memset changed the wrong byte or overwrote its guard.");
                }
                Require(state.gpr[3] == base + start && state.gpr[1] == stack &&
                            state.gpr[31] == 0x98765432u && state.pc == 0x80001000u,
                        "Original memset did not preserve its return value, stack or r31.");
            }
            for (int displacement : {-8, -1, 0, 1, 8, 264}) {
                std::vector<uint8_t> expected(1024);
                for (uint32_t i = 0; i < expected.size(); ++i) {
                    expected[i] = static_cast<uint8_t>(i ^ (i >> 3));
                    ppc_write_u8(state, base + i, expected[i]);
                }
                const uint32_t source = 32u + alignment;
                const uint32_t destination = static_cast<uint32_t>(
                    static_cast<int>(source) + displacement);
                const std::vector<uint8_t> copy(expected.begin() + source,
                                                 expected.begin() + source + count);
                std::copy(copy.begin(), copy.end(), expected.begin() + destination);
                state.gpr[3] = base + destination;
                state.gpr[4] = base + source;
                state.gpr[5] = count;
                state.lr = 0x80001000u;
                ppc_800055CC(state);
                for (uint32_t i = 0; i < expected.size(); ++i) {
                    Require(ppc_read_u8(state, base + i) == expected[i],
                            "Original memcpy failed a direction, overlap or guard test.");
                }
                Require(state.gpr[3] == base + destination && state.gpr[1] == stack &&
                            state.pc == 0x80001000u,
                        "Original memcpy changed its return value or return address.");
            }
        }
    }
    std::cout << "US memcpy/memset passed zero-size, alignment, overlap, guard and return tests.\n";
}

void TestMemoryInitialization(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    BindMemoryFunctions(state);
    struct Range { uint32_t address; uint32_t size; };
    std::vector<Range> bss;
    for (uint32_t table = 0x800056a0u; table < 0x800056c0u; table += 8) {
        const uint32_t size = ppc_read_u32(state, table + 4);
        if (size == 0) {
            break;
        }
        const uint32_t address = ppc_read_u32(state, table);
        bss.push_back({address, size});
        const size_t offset = ppc_memory_offset(state, address, size);
        std::fill_n(state.memory.begin() + offset, size, 0xa5);
        ppc_write_u8(state, address - 1, 0x5a);
        ppc_write_u8(state, address + size, 0x5a);
    }
    Require(bss.size() == 3, "Verified DOL did not contain the expected three BSS ranges.");
    std::vector<std::vector<uint32_t>> text;
    for (const auto& section : image.sections()) {
        if (section.executable) {
            text.push_back(image.ReadWords(section.address, section.size));
        }
    }
    const uint32_t stack = state.gpr[1];
    state.gpr[29] = 0x29292929u;
    state.gpr[30] = 0x30303030u;
    state.gpr[31] = 0x31313131u;
    state.lr = 0x80003170u;
    ppc_80003340(state);
    for (const auto& range : bss) {
        const size_t offset = ppc_memory_offset(state, range.address, range.size);
        Require(std::all_of(state.memory.begin() + offset,
                            state.memory.begin() + offset + range.size,
                            [](uint8_t byte) { return byte == 0; }) &&
                    ppc_read_u8(state, range.address - 1) == 0x5a &&
                    ppc_read_u8(state, range.address + range.size) == 0x5a,
                "Original memory initializer did not clear precisely the BSS range.");
    }
    size_t sectionIndex = 0;
    for (const auto& section : image.sections()) {
        if (!section.executable) {
            continue;
        }
        const auto& expected = text.at(sectionIndex++);
        for (size_t i = 0; i < expected.size(); ++i) {
            Require(ppc_read_u32(state, section.address + static_cast<uint32_t>(i * 4)) ==
                        expected[i], "Memory initialization modified loaded text.");
        }
    }
    Require(state.gpr[1] == stack && state.pc == 0x80003170u &&
                state.gpr[29] == 0x29292929u && state.gpr[30] == 0x30303030u &&
                state.gpr[31] == 0x31313131u,
            "Original memory initializer damaged the ABI or continuation.");
    std::cout << "US ROM/BSS initializer cleared all three original BSS ranges; "
                 "loaded text, guards and callee-saved registers preserved.\n";

    ppc_write_u32(state, 0x8000561cu, 0x80001000u);
    ppc_write_u32(state, 0x80005620u, 0x80002000u);
    ppc_write_u32(state, 0x80005624u, 4);
    ppc_write_u32(state, 0x80001000u, 0x12345678u);
    bool refused = false;
    try {
        ppc_80003340(state);
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) ==
                    "Unsupported startup memory dependency at " +
                        std::to_string(0x80003424u),
                "ROM-copy path failed at an unexpected dependency.");
        refused = true;
    }
    Require(refused && ppc_read_u32(state, 0x80002000u) == 0x12345678u,
            "Relocating ROM-copy path did not copy or silently skipped cache synchronization.");
    std::cout << "Relocating ROM-copy path copies data then explicitly rejects "
                 "the unimplemented cache synchronization service.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("Expected verified US main.dol path.");
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) {
            throw std::runtime_error("Could not open startup test DOL.");
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        Require(Sha1Hex(bytes) == "1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374",
                "Startup tests require the verified US revision 0 DOL.");
        const auto image = DolImage::Parse(std::move(bytes));
        TestEntryAndRegisters(image);
        TestFloatingPointInitialization(image);
        TestPairedInitialization(image);
        TestHardwareFprInitialization(image);
        TestOriginalSystemTime(image);
        TestCompleteHardwareInitialization(image);
        TestEntryThroughHardwareAndMemory(image);
        TestOriginalCacheControls(image);
        TestOriginalFprSaveRestore(image);
        TestOriginalFpscrAccessors(image);
        TestOriginalNonIeeeMode(image);
        TestOriginalSpeculationDisable(image);
        TestOriginalArenaAccessors(image);
        TestOriginalExceptionInitialization(image);
        TestOriginalSystemCallInstallation(image);
        TestOriginalResetRegistration(image);
        TestOriginalAlarmInitialization(image);
        TestOriginalModuleInitialization(image);
        TestOriginalInterruptTableInitialization(image);
        TestOriginalInterruptHandlerAccessors(image);
        TestOriginalInterruptPiSetup(image);
        TestOriginalCacheRanges(image);
        TestOriginalBootInfoAndArena(image);
        TestStartupHelpers(image);
        TestArgumentRelocation(image);
        TestMemoryFunctions(image);
        TestMemoryInitialization(image);
    } catch (const std::exception& error) {
        std::cerr << "US startup test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All compiled US startup tests passed. This is not a game boot.\n";
    return 0;
}
