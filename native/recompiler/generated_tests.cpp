#include "generated_ppc_tests.h"
#include "native_dispatch.h"
#include "native_time_base.h"

#include <algorithm>
#include <cfenv>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestLogicalRegisters() {
    for (uint32_t source : {0u, 1u, 0x80000000u, 0xffffffffu, 0x12345678u}) {
        for (uint32_t right : {0u, 1u, 31u, 32u, 63u, 64u, 0xffffffffu}) {
            for (unsigned operation = 0; operation < 4; ++operation) {
                PpcState state;
                state.gpr[3] = source;
                state.gpr[4] = 0xdeadbeefu;
                state.gpr[5] = right;
                state.xer = 0x80000000u;
                state.cr = 0x01234567u;
                const uint32_t shift = right & 63u;
                uint32_t expected = 0;
                switch (operation) {
                case 0:
                    ppc_00001060(state);
                    expected = shift >= 32 ? 0u : source << shift;
                    break;
                case 1:
                    ppc_00001070(state);
                    expected = source & right;
                    break;
                case 2:
                    ppc_000016F0(state);
                    expected = source | right;
                    break;
                case 3:
                    ppc_00001860(state);
                    expected = shift >= 32 ? 0u : source >> shift;
                    break;
                }
                Require(state.gpr[4] == expected && state.gpr[3] == source &&
                            state.gpr[5] == right,
                        "Compiled logical instruction used incorrect register fields or shift width.");
                const uint32_t cr = expected == 0 ? 3u :
                                    (expected & 0x80000000u) != 0 ? 9u : 5u;
                Require((state.cr >> 28) == cr && (state.cr & 0x0fffffffu) == 0x01234567u,
                        "Compiled logical record instruction produced incorrect CR0.");
            }
        }
    }
}

void TestFloatingPointTransfers() {
    using Function = void (*)(PpcState&);
    const Function transfers[] = {
        ppc_00005000, ppc_00005010, ppc_00005020, ppc_00005030,
        ppc_00005040, ppc_00005050, ppc_00005060, ppc_00005070,
        ppc_00005080, ppc_00005090, ppc_000050A0, ppc_000050B0,
        ppc_000050C0, ppc_000050D0};
    const uint64_t values[] = {
        0, UINT64_C(0x8000000000000000), UINT64_C(0x3ff0000000000000),
        UINT64_C(0xbff8000000000000), 1, UINT64_C(0x000fffffffffffff),
        UINT64_C(0x0010000000000000), UINT64_C(0x7fefffffffffffff),
        UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
        UINT64_C(0x7ff0000000001234), UINT64_C(0xfff8123456789abc)};
    for (uint32_t alias : {0u, 0x80000000u, 0xc0000000u}) {
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.memory.resize(kGameCubeRamSize, 0xcc);
        for (unsigned index = 0; index < 14; ++index) {
            const bool load = index < 8 ? index % 4 < 2 :
                              index == 8 || index == 10 || index == 12;
            const bool update = index == 1 || index == 3 || index == 5 ||
                                index == 7 || index >= 12;
            for (uint64_t bits : values) {
                state.gpr[0] = 0xdeadbeefu;
                state.gpr[3] = index < 4 ? alias + 0x108u :
                               index < 8 ? alias + 0xf8u :
                               index >= 12 ? alias + 0x80u : 0x12345678u;
                state.gpr[4] = index < 8 ? 8u : alias + 0x100u;
                state.cr = 0x12345678u;
                state.xer = 0xe0000000u;
                state.fpscr = 0xfedcba98u;
                for (unsigned reg = 0; reg < 32; ++reg) {
                    state.fpr[reg] = {UINT64_C(0x7ff8000000000000) + reg,
                                      UINT64_C(0x8000000000000000) + reg};
                }
                state.fpr[3].ps0 = load ? ~bits : bits;
                auto expected = state.fpr;
                if (load) {
                    expected[3].ps0 = bits;
                }
                ppc_write_u64(state, alias + 0x100u, load ? bits : ~bits);
                const uint32_t base = state.gpr[3];
                const uint32_t offset = state.gpr[4];
                const uint32_t effective = index >= 12 ? base + base : alias + 0x100u;
                transfers[index](state);
                Require(state.fpr == expected &&
                            state.gpr[3] == (update ? effective : base) &&
                            state.gpr[4] == offset && state.gpr[0] == 0xdeadbeefu &&
                            state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                            state.fpscr == 0xfedcba98u &&
                            state.pc == 0x5004u + index * 16u,
                        "Compiled double transfer changed raw bits, paired lanes, flags or base.");
                Require(ppc_read_u64(state, alias + 0x100u) == bits &&
                            ppc_read_u8(state, alias + 0xffu) == 0xcc &&
                            ppc_read_u8(state, alias + 0x108u) == 0xcc,
                        "Compiled double transfer lost big-endian bytes or overwrote guards.");
            }
        }
        state.gpr[3] = alias + 0x10cu;
        state.fpr[3].ps0 = UINT64_C(0x7ff0123456789abc);
        ppc_00005020(state);
        state.fpr[3].ps0 = 0;
        ppc_00005000(state);
        Require(state.fpr[3].ps0 == UINT64_C(0x7ff0123456789abc),
                "Double transfer incorrectly required eight-byte rather than word alignment.");
        for (unsigned index = 0; index < 8; ++index) {
            for (uint32_t address : {0x101u, 0x102u, 0x103u,
                                     0x817ffffcu, 0xcc000000u, 0xfffffffcu}) {
                state.gpr[3] = index < 4 ? address + 8u : address;
                state.gpr[4] = 0;
                const uint32_t base = state.gpr[3];
                const auto before = state.fpr;
                const auto memory = state.memory;
                bool rejected = false;
                try {
                    transfers[index](state);
                } catch (const std::runtime_error& error) {
                    Require((address & 3u) != 0 &&
                                std::string(error.what()).find("Unaligned PowerPC") !=
                                    std::string::npos,
                            "Double transfer failed for an unexpected non-memory reason.");
                    rejected = true;
                } catch (const std::out_of_range&) {
                    rejected = true;
                }
                Require(rejected && state.fpr == before && state.memory == memory &&
                            state.gpr[3] == base,
                        "Invalid double transfer partially updated memory, FPR or base.");
            }
        }
    }
}

void TestFloatingPointMoves() {
    using Function = void (*)(PpcState&);
    const Function functions[] = {
        ppc_00005200, ppc_00005210, ppc_00005220, ppc_00005230,
        ppc_00005240, ppc_00005250, ppc_00005260, ppc_00005270,
        ppc_00005280, ppc_00005290, ppc_000052A0, ppc_000052B0,
        ppc_000052C0, ppc_000052D0, ppc_000052E0, ppc_000052F0};
    for (unsigned operation = 0; operation < 16; ++operation) {
        for (uint64_t bits : {UINT64_C(0), UINT64_C(0x8000000000000000),
                             UINT64_C(1), UINT64_C(0x3ff0000000000000),
                             UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
                             UINT64_C(0x7ff0123456789abc), UINT64_C(0xfff8123456789abc)}) {
            for (uint32_t flags : {0u, 0xf1234567u, 0x81234567u, 0x12345678u}) {
                PpcState state;
                for (unsigned reg = 0; reg < 32; ++reg) {
                    state.gpr[reg] = reg + 0x100;
                    state.fpr[reg] = {reg + 0x200u, reg + 0x300u};
                }
                const unsigned source = (operation & 1u) ? 3u : 4u;
                state.fpr[source].ps0 = bits;
                auto expected = state.fpr;
                const uint64_t sign = UINT64_C(0x8000000000000000);
                expected[3].ps0 = operation / 4 == 0 ? bits :
                                  operation / 4 == 1 ? bits ^ sign :
                                  operation / 4 == 2 ? bits & ~sign : bits | sign;
                state.fpscr = flags;
                state.cr = 0x12345678u;
                state.xer = 0xe0000000u;
                functions[operation](state);
                const uint32_t cr = (operation & 2u) ?
                    (0x10345678u | ((flags >> 4) & 0x0f000000u)) : 0x12345678u;
                Require(state.fpr == expected && state.fpscr == flags &&
                            state.cr == cr && state.xer == 0xe0000000u,
                        "Compiled FPR move/sign operation changed payload, paired lane or CR1.");
                for (unsigned reg = 0; reg < 32; ++reg) {
                    Require(state.gpr[reg] == reg + 0x100,
                            "Compiled floating-point move changed an integer register.");
                }
            }
        }
    }
}

uint32_t ExpectedFpscr(uint32_t original, uint32_t source, uint32_t fieldMask) {
    uint32_t result = original;
    for (unsigned field = 0; field < 8; ++field) {
        if ((fieldMask & (0x80u >> field)) != 0) {
            const uint32_t nibble = 0xf0000000u >> (field * 4u);
            result = (result & ~nibble) | (source & nibble);
        }
    }
    bool invalid = false;
    for (unsigned bit : {7u, 8u, 9u, 10u, 11u, 12u, 21u, 22u, 23u}) {
        invalid = invalid || (result & (0x80000000u >> bit)) != 0;
    }
    result = (result & ~0x20000000u) | (invalid ? 0x20000000u : 0);
    bool enabledException = false;
    for (unsigned kind = 0; kind < 5; ++kind) {
        enabledException = enabledException ||
            ((result & (0x20000000u >> kind)) != 0 &&
             (result & (0x80u >> kind)) != 0);
    }
    return (result & ~0x40000000u) | (enabledException ? 0x40000000u : 0);
}

void TestFpscrWrites() {
    const int rounding = std::fegetround();
    const uint32_t values[] = {
        0, 0xffffffffu, 0x80000000u, 0x60000000u, 0x01f80700u,
        0x20000080u, 0x01000080u, 0x10000040u, 0x08000020u,
        0x04000010u, 0x02000008u, 0x12345678u, 0xfedcba98u,
        0x00000001u, 0x00000002u, 0x00000003u, 0x00000004u};
    const auto check = [](unsigned function, uint32_t initial, uint32_t source,
                          uint32_t expected, bool record) {
        PpcState state;
        for (unsigned reg = 0; reg < 32; ++reg) {
            state.gpr[reg] = reg + 0x100;
            state.fpr[reg] = {UINT64_C(0x7ff0000000000000) + reg,
                              UINT64_C(0xfff8000000000000) + reg};
        }
        state.fpr[4].ps0 = UINT64_C(0xdeadbeef00000000) | source;
        const auto fpr = state.fpr;
        state.fpscr = initial;
        state.cr = 0x12345678u;
        state.xer = 0xe0000000u;
        state.lr = 0x98765430u;
        state.ctr = 0x87654321u;
        FpscrTestFunctions[function](state);
        const uint32_t cr = record ?
            (0x10345678u | ((expected >> 4) & 0x0f000000u)) : 0x12345678u;
        Require(state.fpscr == expected && state.cr == cr &&
                    state.fpr == fpr && state.xer == 0xe0000000u &&
                    state.lr == 0x98765430u && state.ctr == 0x87654321u &&
                    state.pc == 0x6004u + function * 4u && state.memory.empty(),
                "FPSCR write changed field masks, summaries, CR1, FPR lanes or unrelated state.");
        for (unsigned reg = 0; reg < 32; ++reg) {
            Require(state.gpr[reg] == reg + 0x100,
                    "FPSCR write changed an integer register.");
        }
    };
    for (unsigned mask = 0; mask < 256; ++mask) {
        for (unsigned record = 0; record < 2; ++record) {
            for (uint32_t initial : values) {
                for (uint32_t source : values) {
                    check(mask * 2 + record, initial, source,
                          ExpectedFpscr(initial, source, mask), record != 0);
                }
            }
            for (unsigned bit = 0; bit < 32; ++bit) {
                const uint32_t value = 1u << bit;
                for (uint32_t initial : {0u, 0xffffffffu}) {
                    check(mask * 2 + record, initial, value,
                          ExpectedFpscr(initial, value, mask), record != 0);
                }
            }
        }
    }
    for (unsigned field = 0; field < 8; ++field) {
        for (unsigned immediate = 0; immediate < 16; ++immediate) {
            for (unsigned record = 0; record < 2; ++record) {
                for (uint32_t initial : values) {
                    check(512u + field * 32u + immediate * 2u + record,
                          initial, 0xdeadbeefu,
                          ExpectedFpscr(initial, immediate << (28u - field * 4u),
                                        0x80u >> field), record != 0);
                }
            }
            Require(std::fegetround() == rounding,
                    "Guest FPSCR writes changed the host floating-point rounding mode.");
        }
    }
}

void TestPerformanceMonitorResets() {
    for (unsigned index = 0; index < 6; ++index) {
        PpcState state;
        for (unsigned reg = 0; reg < 32; ++reg) state.gpr[reg] = reg + 0x100u;
        state.gpr[3] = 0;
        state.fpr.fill({UINT64_MAX, UINT64_MAX});
        state.gqr.fill(0x12345678u);
        state.performanceMonitor.fill(0x87654321u);
        state.memory.resize(64, 0xa5);
        state.cr = 0x12345678u;
        state.xer = 0xe0000000u;
        state.lr = 0x80001000u;
        state.ctr = 0x11111111u;
        state.fpscr = 4;
        state.msr = 0x2030u;
        state.hid0 = 0xc000u;
        state.hid2 = 0xa0000000u;
        state.l2cr = 0x80000000u;
        const auto before = state;
        auto expected = state.performanceMonitor;
        expected[index] = 0;
        const auto check = [&] {
            Require(state.memory == before.memory && state.fpr == before.fpr &&
                        state.gqr == before.gqr && state.cr == before.cr &&
                        state.xer == before.xer && state.lr == before.lr &&
                        state.ctr == before.ctr && state.fpscr == before.fpscr &&
                        state.msr == before.msr && state.hid0 == before.hid0 &&
                        state.hid2 == before.hid2 && state.l2cr == before.l2cr,
                    "Performance-monitor reset changed unrelated CPU state or RAM.");
            for (unsigned reg = 0; reg < 32; ++reg) {
                if (reg == 3) continue;
                Require(state.gpr[reg] == before.gpr[reg],
                        "Performance-monitor reset changed an integer register.");
            }
        };
        for (bool interrupts : {false, true}) {
            state.externalInterruptsEnabled = interrupts;
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                PerformanceResetTestFunctions[index](state);
                Require(state.performanceMonitor == expected &&
                            state.pc == 0xb004u + index * 16u && state.gpr[3] == 0 &&
                            state.externalInterruptsEnabled == interrupts,
                        "Performance reset targeted the wrong register or changed interrupt state.");
                check();
            }
        }
        for (unsigned bit = 0; bit < 32; ++bit) {
            state.gpr[3] = 1u << bit;
            state.pc = 0xb000u + index * 16u;
            bool rejected = false;
            try {
                PerformanceResetTestFunctions[index](state);
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("only zero resets") != std::string::npos;
            }
            Require(rejected && state.performanceMonitor == expected &&
                        state.pc == 0xb000u + index * 16u && state.gpr[3] == (1u << bit) &&
                        state.externalInterruptsEnabled,
                    "Unsupported performance-monitor mode did not fail before mutation.");
            check();
        }
    }
}

void TestFpscrBits() {
    const int rounding = std::fegetround();
    const unsigned exceptionBits[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 21, 22, 23};
    for (unsigned set = 0; set < 2; ++set) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            const uint32_t mask = 0x80000000u >> bit;
            for (unsigned record = 0; record < 2; ++record) {
                for (uint32_t initial : {0u, UINT32_MAX, 0x12345678u, 0xfedcba98u,
                                        mask, ~mask, 0x60000000u, 0x01f80700u,
                                        0x000000f8u, 0x01000080u, 0x10000040u}) {
                    PpcState state;
                    for (unsigned reg = 0; reg < 32; ++reg) {
                        state.gpr[reg] = 0x100u + reg;
                        state.fpr[reg] = {UINT64_C(0x7ff0000000000001) + reg,
                                          UINT64_C(0xfff8000000000000) + reg};
                    }
                    const auto fpr = state.fpr;
                    state.memory.resize(32, 0xa5);
                    const auto memory = state.memory;
                    state.fpscr = initial;
                    state.cr = 0x12345678u;
                    state.xer = 0xe0000000u;
                    state.lr = 0x80001000u;
                    state.ctr = 0x87654321u;
                    uint32_t value = set ? initial | mask : initial & ~mask;
                    const bool exception = std::find(std::begin(exceptionBits),
                        std::end(exceptionBits), bit) != std::end(exceptionBits);
                    if (set && exception && (initial & mask) == 0) value |= 0x80000000u;
                    const uint32_t expected = ExpectedFpscr(0, value, 255);
                    const unsigned index = set * 64 + bit * 2 + record;
                    for (unsigned repeat = 0; repeat < 2; ++repeat) {
                        FpscrBitTestFunctions[index](state);
                        Require(state.fpscr == expected &&
                                    state.cr == (record ? (0x10345678u |
                                        ((expected >> 4) & 0x0f000000u)) : 0x12345678u) &&
                                    state.fpr == fpr && state.memory == memory &&
                                    state.xer == 0xe0000000u && state.lr == 0x80001000u &&
                                    state.ctr == 0x87654321u &&
                                    state.pc == 0xa004u + index * 4,
                                "mtfsb0/mtfsb1 lost bit, FX/VX/FEX, CR1 or repeat semantics.");
                    }
                    for (unsigned reg = 0; reg < 32; ++reg) {
                        Require(state.gpr[reg] == 0x100u + reg,
                                "FPSCR bit write changed an integer register.");
                    }
                }
            }
        }
    }
    Require(std::fegetround() == rounding,
            "FPSCR bit operations changed the host rounding mode.");
}

void TestFpscrReads() {
    for (uint32_t flags : {0u, 0xffffffffu, 0x12345678u, 0xfedcba98u,
                          0x20000000u, 0x40000000u}) {
        for (bool record : {false, true}) {
            PpcState state;
            for (unsigned reg = 0; reg < 32; ++reg) {
                state.fpr[reg] = {UINT64_C(0x7ff0000000000001) + reg,
                                  UINT64_C(0x8000000000000000) + reg};
            }
            auto expected = state.fpr;
            expected[3].ps0 = UINT64_C(0xfff8000000000000) | flags;
            state.fpscr = flags;
            state.cr = 0x12345678u;
            state.xer = 0xe0000000u;
            (record ? ppc_00007010 : ppc_00007000)(state);
            const uint32_t cr = record ?
                (0x10345678u | ((flags >> 4) & 0x0f000000u)) : 0x12345678u;
            Require(state.fpr == expected && state.fpscr == flags &&
                        state.cr == cr && state.xer == 0xe0000000u &&
                        state.pc == (record ? 0x7014u : 0x7004u),
                    "mffs lost boxed FPSCR bits, changed summaries, paired lanes or CR1.");
        }
    }
}

void TestSingleWidening() {
    const std::pair<uint32_t, uint64_t> cases[] = {
        {0, 0}, {0x80000000u, UINT64_C(0x8000000000000000)},
        {0x3f800000u, UINT64_C(0x3ff0000000000000)},
        {1, UINT64_C(0x36a0000000000000)},
        {0x7fffffu, UINT64_C(0x380fffffc0000000)},
        {0x800000u, UINT64_C(0x3810000000000000)},
        {0x7f7fffffu, UINT64_C(0x47efffffe0000000)},
        {0x7f800000u, UINT64_C(0x7ff0000000000000)},
        {0xff800000u, UINT64_C(0xfff0000000000000)},
        {0x7f800001u, UINT64_C(0x7ff0000020000000)},
        {0xffc12345u, UINT64_C(0xfff82468a0000000)}};
    for (const auto& value : cases) {
        Require(ppc_widen_single_bits(value.first) == value.second,
                "Single widening lost a signed zero, subnormal, infinity or NaN payload.");
    }
    uint32_t bits = 0x12345678u;
    for (unsigned i = 0; i < 65536; ++i) {
        bits = bits * 1664525u + 1013904223u;
        if ((bits & 0x7f800000u) == 0x7f800000u) {
            continue;
        }
        float single;
        std::memcpy(&single, &bits, sizeof(single));
        const double reference = single;
        uint64_t expected;
        std::memcpy(&expected, &reference, sizeof(expected));
        Require(ppc_widen_single_bits(bits) == expected,
                "Integer-only single widening differs from an exact finite conversion.");
    }
    for (unsigned bit = 0; bit < 23; ++bit) {
        for (uint32_t sign : {0u, 0x80000000u}) {
            const uint32_t value = sign | (1u << bit);
            const uint64_t expected = (static_cast<uint64_t>(sign) << 32) |
                                     (static_cast<uint64_t>(874u + bit) << 52);
            Require(ppc_widen_single_bits(value) == expected,
                    "Subnormal widening failed at a normalization boundary.");
        }
    }
}

void TestPairedLoads(bool indexed) {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    state.gpr[4] = 0xfffffffcu;
    const auto* functions = indexed ? IndexedPairedLoadTestFunctions : PairedLoadTestFunctions;
    const uint32_t entry = indexed ? 0x9000u : 0x8100u;
    const uint32_t values[] = {0, 0x80000000u, 1, 0x7fffffu, 0x800000u,
        0x3f800000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u, 0x7f812345u, 0xffc12345u};
    for (unsigned index = 0; index < 32; ++index) {
        const unsigned quantization = index / 4;
        const bool single = (index & 2u) != 0;
        const bool update = (index & 1u) != 0;
        for (uint32_t alias : {0u, 0x80000000u, 0xc0000000u}) {
            for (uint32_t first : values) {
                for (uint32_t second : values) {
                    state.gqr.fill(0x00040000u);
                    // Type zero ignores load scale and unrelated store quantization.
                    state.gqr[quantization] = 0x3f00ffffu;
                    state.gpr[3] = alias + 0x104u;
                    state.gpr[0] = 0xdeadbeefu;
                    state.fpr[3] = {UINT64_MAX, UINT64_MAX};
                    auto expected = state.fpr;
                    expected[3] = {ppc_widen_single_bits(first),
                        single ? UINT64_C(0x3ff0000000000000) : ppc_widen_single_bits(second)};
                    state.fpscr = 0xfedcba98u;
                    state.cr = 0x12345678u;
                    state.xer = 0xe0000000u;
                    const auto gqr = state.gqr;
                    ppc_write_u32(state, alias + 0x100u, first);
                    ppc_write_u32(state, alias + 0x104u, second);
                    functions[index](state);
                    Require(state.fpr == expected && state.gqr == gqr &&
                                state.gpr[3] == alias + (update ? 0x100u : 0x104u) &&
                                state.gpr[0] == 0xdeadbeefu &&
                                state.fpscr == 0xfedcba98u && state.cr == 0x12345678u &&
                                state.xer == 0xe0000000u && state.pc == entry + 4u + index * 16u &&
                                state.gpr[4] == 0xfffffffcu &&
                                ppc_read_u32(state, alias + 0x100u) == first &&
                                ppc_read_u32(state, alias + 0x104u) == second,
                            "Paired load lost lanes, W/I/displacement fields, flags or base update.");
                }
            }
        }
        for (uint32_t address : {0x817ffffcu, 0x817ffff8u, 0xcc000000u, 0xfffffffcu}) {
            state.gqr[quantization] = 0;
            state.gpr[3] = address + 4u;
            const auto before = state.fpr;
            if (address < 0x81800000u && (address == 0x817ffff8u || single)) {
                functions[index](state);
            } else {
                bool rejected = false;
                try {
                    functions[index](state);
                } catch (const std::out_of_range&) {
                    rejected = true;
                }
                Require(rejected && state.fpr == before && state.gpr[3] == address + 4u,
                        "Out-of-range paired load partially updated a lane or its base.");
            }
        }
        for (uint32_t type = 1; type < 4; ++type) {
            state.gqr[quantization] = type << 16;
            state.gpr[3] = 0x104u;
            const auto before = state.fpr;
            bool rejected = false;
            try {
                functions[index](state);
            } catch (const std::runtime_error& error) {
                Require(std::string(error.what()).find("quantized load type") != std::string::npos,
                        "Unsupported paired quantization failed for an unexpected reason.");
                rejected = true;
            }
            Require(rejected && state.fpr == before && state.gpr[3] == 0x104u,
                    "Unsupported paired quantization silently succeeded or changed registers.");
        }
    }
    state.gqr[0] = 0;
    state.gpr[0] = 0xcc000000u;
    ppc_write_u32(state, 0x100u, 0x3f800000u);
    ppc_write_u32(state, 0x104u, 0xbf800000u);
    ppc_00008400(state);
    Require(state.fpr[3] == PpcFpr{UINT64_C(0x3ff0000000000000),
                                  UINT64_C(0xbff0000000000000)},
            "psq_l RA=0 did not use the architectural zero base.");
    ppc_write_u32(state, 0x101u, 0x80000000u);
    ppc_write_u32(state, 0x105u, 0x7f800001u);
    state.gpr[3] = 0x105u;
    const auto before = state.memory;
    PairedLoadTestFunctions[0](state);
    Require(state.fpr[3] == PpcFpr{UINT64_C(0x8000000000000000),
                                  UINT64_C(0x7ff0000020000000)} && state.memory == before,
            "Unquantized paired load did not preserve unaligned big-endian data and RAM guards.");
}

void TestPairedMoves() {
    const uint64_t values[] = {0, UINT64_C(0x8000000000000000), 1,
        UINT64_C(0x3ff0000000000000), UINT64_C(0x7ff0000000000000),
        UINT64_C(0xfff0000000000000), UINT64_C(0x7ff0000000001234),
        UINT64_C(0xfff8123456789abc)};
    for (unsigned index = 0; index < 48; ++index) {
        const unsigned operation = index / 6;
        const bool record = ((index / 3) & 1u) != 0;
        const unsigned alias = index % 3;
        const unsigned left = alias == 1 ? 3 : 4;
        const unsigned right = alias == 2 ? 3 : 5;
        for (uint64_t first : values) {
            for (uint64_t second : values) {
                PpcState state;
                state.fpr[3] = {0x1234, 0x5678};
                state.fpr[left] = {first, second};
                state.fpr[right] = {second, first};
                auto expected = state.fpr;
                if (operation < 4) {
                    const uint64_t sign = UINT64_C(0x8000000000000000);
                    const auto transform = [operation, sign](uint64_t value) {
                        return operation == 0 ? value : operation == 1 ? value ^ sign :
                               operation == 2 ? value & ~sign : value | sign;
                    };
                    expected[3] = {transform(state.fpr[right].ps0), transform(state.fpr[right].ps1)};
                } else {
                    expected[3] = {
                        operation < 6 ? state.fpr[left].ps0 : state.fpr[left].ps1,
                        (operation & 1u) ? state.fpr[right].ps1 : state.fpr[right].ps0};
                }
                state.fpscr = 0x91234567u;
                state.cr = 0x12345678u;
                state.xer = 0xe0000000u;
                PairedMoveTestFunctions[index](state);
                Require(state.fpr == expected && state.fpscr == 0x91234567u &&
                            state.cr == (record ? 0x19345678u : 0x12345678u) &&
                            state.xer == 0xe0000000u && state.pc == 0x8504u + index * 16u,
                        "Paired move/merge changed raw bits, alias order, FPSCR or CR1.");
            }
        }
    }
}

uint64_t ReferenceDequantization(uint32_t raw, unsigned width, bool signedValue,
                                  unsigned scale) {
    const uint32_t mask = (1u << width) - 1;
    const int value = signedValue && (raw & (1u << (width - 1))) != 0 ?
        static_cast<int>(raw & mask) - static_cast<int>(1u << width) :
        static_cast<int>(raw & mask);
    const int exponent = scale < 32 ? -static_cast<int>(scale) :
                                               64 - static_cast<int>(scale);
    const double result = std::ldexp(static_cast<double>(value), exponent);
    uint64_t bits;
    std::memcpy(&bits, &result, sizeof(bits));
    return bits;
}

void TestIntegerDequantization() {
    const int rounding = std::fegetround();
    for (uint32_t type = 4; type < 8; ++type) {
        const unsigned width = (type & 1u) != 0 ? 16 : 8;
        const bool signedValue = (type & 2u) != 0;
        for (unsigned scale = 0; scale < 64; ++scale) {
            for (uint32_t value = 0; value < (1u << width); ++value) {
                Require(ppc_dequantize_integer_bits(value, type, scale) ==
                            ReferenceDequantization(value, width, signedValue, scale),
                        "Integer dequantization differs at an integer/scale/sign boundary.");
            }
        }
    }
    Require(std::fegetround() == rounding, "Integer dequantization changed host rounding mode.");
}

void TestCompiledQuantizedLoads(bool indexed) {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    state.gpr[4] = 0xfffffffcu;
    const auto* functions = indexed ? IndexedPairedLoadTestFunctions : PairedLoadTestFunctions;
    const uint32_t entry = indexed ? 0x9000u : 0x8100u;
    const uint32_t values[] = {0, 1, 0x7f, 0x80, 0xff, 0x100,
                              0x7fff, 0x8000, 0xffff, 0x1234, 0xfedc};
    const auto write = [&](uint32_t address, uint32_t value, unsigned width) {
        if (width == 16) {
            ppc_write_u16(state, address, static_cast<uint16_t>(value));
        } else {
            ppc_write_u8(state, address, static_cast<uint8_t>(value));
        }
    };
    for (unsigned index = 0; index < 32; ++index) {
        const unsigned selector = index / 4;
        const bool single = (index & 2u) != 0;
        const bool update = (index & 1u) != 0;
        for (uint32_t type = 4; type < 8; ++type) {
            const unsigned width = (type & 1u) != 0 ? 16 : 8;
            const bool signedValue = (type & 2u) != 0;
            const uint32_t stride = width / 8;
            for (unsigned scale = 0; scale < 64; ++scale) {
                for (uint32_t alias : {0u, 0x80000000u, 0xc0000000u}) {
                    for (uint32_t raw : values) {
                        const uint32_t second = ~raw & ((1u << width) - 1u);
                        const uint32_t target = alias + 0x101u;
                        state.gqr.fill(0x00010000u);
                        state.gqr[selector] = (scale << 24) | (type << 16) | 0xffffu;
                        state.gpr[3] = target + 4u;
                        state.gpr[0] = 0xdeadbeefu;
                        state.fpr[3] = {UINT64_MAX, UINT64_MAX};
                        state.cr = 0x12345678u;
                        state.xer = 0xe0000000u;
                        state.fpscr = 0xfedcba98u;
                        const auto quantization = state.gqr;
                        auto expected = state.fpr;
                        expected[3] = {ReferenceDequantization(raw, width, signedValue, scale),
                            single ? UINT64_C(0x3ff0000000000000) :
                                     ReferenceDequantization(second, width, signedValue, scale)};
                        write(target, raw, width);
                        write(target + stride, second, width);
                        const auto source = ppc_read_u32(state, target);
                        functions[index](state);
                        Require(state.fpr == expected && state.gqr == quantization &&
                                    state.gpr[3] == target + (update ? 0u : 4u) &&
                                    state.gpr[0] == 0xdeadbeefu &&
                                    state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                                    state.fpscr == 0xfedcba98u &&
                                    state.pc == entry + 4u + index * 16u &&
                                    state.gpr[4] == 0xfffffffcu &&
                                    ppc_read_u32(state, target) == source,
                                "Compiled quantized load changed scaling, width, lanes, ABI or source.");
                    }
                }
            }
            state.gqr[selector] = type << 16;
            const uint32_t bytes = single ? stride : stride * 2u;
            const uint32_t last = 0x81800000u - bytes;
            state.gpr[3] = last + 4u;
            functions[index](state);
            for (uint32_t address : {last + 1u, 0xcc000000u, 0xffffffffu}) {
                state.gpr[3] = address + 4u;
                const auto fpr = state.fpr;
                bool rejected = false;
                try {
                    functions[index](state);
                } catch (const std::out_of_range&) {
                    rejected = true;
                }
                Require(rejected && state.fpr == fpr && state.gpr[3] == address + 4u,
                        "Quantized load boundary error partially changed a lane or base.");
            }
        }
    }
    state.gqr[0] = 7u << 16;
    ppc_write_u16(state, 0x100, 0x8000);
    ppc_write_u16(state, 0x102, 0x7fff);
    state.gpr[0] = 0xcc000000u;
    ppc_00008400(state);
    Require(state.fpr[3] == PpcFpr{UINT64_C(0xc0e0000000000000),
                                  UINT64_C(0x40dfffc000000000)},
            "Quantized RA=0 load did not preserve signed 16-bit extremes.");
}

void TestQuantizationRegisters() {
    using Function = void (*)(PpcState&);
    const Function functions[] = {ppc_00008000, ppc_00008010, ppc_00008020, ppc_00008030,
                                  ppc_00008040, ppc_00008050, ppc_00008060, ppc_00008070};
    for (unsigned reg = 0; reg < 8; ++reg) {
        for (uint32_t value : {0u, 0xffffffffu, 0x12345678u, 0x80000000u}) {
            PpcState state;
            state.gqr.fill(0xdeadbeefu);
            auto expected = state.gqr;
            expected[reg] = value;
            state.gpr[3] = value;
            state.cr = 0x12345678u;
            state.xer = 0xe0000000u;
            functions[reg](state);
            Require(state.gqr == expected && state.gpr[4] == value &&
                        state.gpr[3] == value && state.cr == 0x12345678u &&
                        state.xer == 0xe0000000u,
                    "Quantization-register transfers changed the wrong SPR or flags.");
        }
    }
}

void TestHardwareControlRegisters() {
    PpcState state;
    state.memory.resize(64, 0xa5);
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 0xf1234567u;
    const auto memory = state.memory;
    for (uint32_t initial : {0u, 0x2000u, 0x30u, 0x2030u}) {
        for (bool interrupts : {false, true}) {
            state.msr = initial;
            state.externalInterruptsEnabled = interrupts;
            ppc_00009300(state);
            Require(state.gpr[3] == (initial | (interrupts ? 0x8000u : 0u)),
                    "mfmsr does not reflect the native interrupt mask.");
            for (uint32_t mode : {0u, 0x2000u, 0x8000u, 0xa000u}) {
                const uint32_t desired = mode | (initial & ~0xa000u);
                state.gpr[3] = desired;
                ppc_00009310(state);
                ppc_00009300(state);
                Require(state.gpr[3] == desired && state.msr == (desired & ~0x8000u) &&
                            state.externalInterruptsEnabled == ((desired & 0x8000u) != 0),
                        "Compiled MSR transfer did not preserve FP/EE state.");
            }
        }
    }
    for (uint32_t initial : {0u, 0x20000000u, 0x80000000u, 0xa0000000u}) {
        state.hid2 = initial;
        ppc_00009320(state);
        Require(state.gpr[3] == initial, "mfspr HID2 did not read guest feature state.");
        for (uint32_t desired : {0u, 0x20000000u, 0x80000000u, 0xa0000000u}) {
            state.gpr[3] = desired;
            ppc_00009330(state);
            ppc_00009320(state);
            Require(state.gpr[3] == desired && state.hid2 == desired,
                    "Compiled HID2 transfer did not preserve PSE/LSQE state.");
        }
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        const uint32_t flag = 1u << bit;
        for (bool msr : {false, true}) {
            const uint32_t allowed = msr ? 0xa000u : 0xa0000000u;
            if ((flag & allowed) != 0) continue;
            for (bool clearing : {false, true}) {
                state.msr = clearing ? flag : 0u;
                state.externalInterruptsEnabled = false;
                state.hid2 = clearing ? flag : 0u;
                state.gpr[3] = clearing ? 0u : flag;
                const auto beforeMsr = state.msr;
                const auto beforeHid2 = state.hid2;
                bool rejected = false;
                try {
                    if (msr) ppc_00009310(state);
                    else ppc_00009330(state);
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                Require(rejected && state.msr == beforeMsr && state.hid2 == beforeHid2 &&
                            !state.externalInterruptsEnabled,
                        "Unsupported control-register mode change was not rejected atomically.");
            }
        }
    }
    Require(state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                state.fpscr == 0xf1234567u && state.memory == memory,
            "Control-register transfer changed flags, FPSCR or RAM.");
}

void TestCacheBlockInstructions() {
    for (auto function : {ppc_0000B100, ppc_0000B110, ppc_0000B120, ppc_0000B130}) {
        const bool indexed = function == ppc_0000B110 || function == ppc_0000B130;
        const uint32_t pc = function == ppc_0000B100 ? 0xb100u :
            function == ppc_0000B110 ? 0xb110u : function == ppc_0000B120 ? 0xb120u : 0xb130u;
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.memory.resize(kGameCubeRamSize, 0xa5);
        state.gpr[0] = UINT32_MAX;
        state.cr = 0x12345678u;
        state.xer = 0xe0000000u;
        state.fpscr = 4;
        const auto memory = state.memory;
        for (uint32_t base : {0u, 0x80000000u, 0xc0000000u}) {
            for (uint32_t offset : {0u, 1u, 31u, 32u, kGameCubeRamSize - 1}) {
                state.gpr[3] = indexed ? base + offset - 0x100u : base + offset;
                state.gpr[4] = 0x100;
                const auto registers = std::vector<uint32_t>(std::begin(state.gpr), std::end(state.gpr));
                function(state);
                Require(state.pc == pc + 4 && state.memory == memory &&
                            std::equal(registers.begin(), registers.end(), std::begin(state.gpr)) &&
                            state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                            state.fpscr == 4,
                        "Cache block operation changed RAM/registers or failed alias/wrapping addressing.");
            }
        }
        for (uint32_t invalid : {kGameCubeRamSize, 0x81800000u, 0xcc000000u, UINT32_MAX}) {
            state.gpr[3] = indexed ? invalid - 0x100u : invalid;
            state.pc = pc;
            bool rejected = false;
            try {
                function(state);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            Require(rejected && state.pc == pc && state.memory == memory,
                    "Unmapped cache operation did not reject before changing RAM/PC.");
        }
    }
}

void TestAndComplement() {
    const auto functions = {ppc_0000B300, ppc_0000B310, ppc_0000B320,
                            ppc_0000B330, ppc_0000B340, ppc_0000B350};
    unsigned index = 0;
    for (auto function : functions) {
        const unsigned destination = index % 3 == 0 ? 4 : index % 3 == 1 ? 3 : 5;
        for (uint32_t left : {0u, 1u, 0x80000000u, 0x55555555u, UINT32_MAX}) {
            for (uint32_t right : {0u, 1u, 0x80000000u, 0xaaaaaaaau, UINT32_MAX}) {
                for (uint32_t xer : {0u, 0xe0000000u}) {
                    PpcState state;
                    state.gpr[3] = left;
                    state.gpr[4] = 0x12345678u;
                    state.gpr[5] = right;
                    state.cr = 0x12345678u;
                    state.xer = xer;
                    const auto before = state;
                    function(state);
                    const uint32_t result = left & ~right;
                    const uint32_t flags = (result == 0 ? 2u :
                        (result & 0x80000000u) != 0 ? 8u : 4u) | (xer >> 31);
                    Require(state.gpr[destination] == result && state.xer == xer &&
                                state.cr == (index < 3 ? before.cr :
                                    (before.cr & 0x0fffffffu) | (flags << 28)) &&
                                state.pc == 0xb304u + index * 16,
                            "Compiled andc failed aliasing, CR0/SO, XER or PC.");
                    for (unsigned reg = 0; reg < 32; ++reg) {
                        if (reg == destination) continue;
                        Require(state.gpr[reg] == before.gpr[reg],
                                "Compiled andc changed an unrelated register.");
                    }
                }
            }
        }
        ++index;
    }
}

void TestExiTransferControl() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    const auto ram = state.memory;
    const auto rejectAccess = [&](auto operation) {
        const auto before = state.exiTransferControl;
        bool rejected = false;
        try {
            operation();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        Require(rejected && state.exiTransferControl == before && state.memory == ram,
                "Unsupported EXI transfer access changed state.");
    };
    for (unsigned channel = 0; channel < 3; ++channel) {
        const uint32_t address = 0xcc00680cu + channel * 0x14u;
        state.gpr[4] = address;
        rejectAccess([&] { ppc_0000B200(state); });
        for (uint32_t control = 0; control <= 0x3e; control += 2) {
            state.exiTransferControl[channel] = control;
            state.cr = 0x12345678u;
            state.xer = 0xe0000000u;
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                ppc_0000B200(state);
                Require(state.gpr[3] == control && state.pc == 0xb204u &&
                            state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                            state.exiTransferControl[channel] == control && state.memory == ram,
                        "Compiled idle EXI transfer read changed state or returned wrong bits.");
            }
        }
        for (unsigned bit = 0; bit < 32; ++bit) {
            if ((0x3eu & (1u << bit)) != 0) continue;
            const uint32_t invalid = 0x3eu | (1u << bit);
            state.exiTransferControl[channel] = invalid;
            state.pc = 0xb200u;
            bool rejected = false;
            try {
                ppc_0000B200(state);
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("only idle state") != std::string::npos;
            }
            Require(rejected && state.pc == 0xb200u && state.memory == ram &&
                        state.exiTransferControl[channel] == invalid,
                    "Busy/reserved EXI transfer state was accepted or changed.");
        }
        state.exiTransferControl[channel] = 0x3e;
        for (uint32_t value : {0u, 1u, 0x3eu, UINT32_MAX}) {
            state.gpr[3] = value;
            rejectAccess([&] { ppc_0000B210(state); });
        }
        for (uint32_t offset : {1u, 2u, 3u, 4u}) {
            state.gpr[4] = address + offset;
            rejectAccess([&] { ppc_0000B200(state); });
        }
        state.gpr[4] = address - 0xc0000000u;
        rejectAccess([&] { ppc_0000B200(state); });
        for (unsigned width : {1u, 2u, 8u}) {
            rejectAccess([&] {
                if (width == 1) (void)ppc_read_u8(state, address);
                else if (width == 2) (void)ppc_read_u16(state, address);
                else (void)ppc_read_u64(state, address);
            });
            rejectAccess([&] {
                if (width == 1) ppc_write_u8(state, address, 0);
                else if (width == 2) ppc_write_u16(state, address, 0);
                else ppc_write_u64(state, address, 0);
            });
        }
        state.exiTransferControl[channel].reset();
        state.gpr[4] = address;
        rejectAccess([&] { ppc_0000B200(state); });
    }
    state.exiTransferControl = {0u, 2u, 0x3eu};
    const auto controls = state.exiTransferControl;
    state.gpr[4] = 0xc0001000u;
    state.gpr[3] = 0x12345678u;
    ppc_0000B210(state);
    ppc_0000B200(state);
    Require(state.gpr[3] == 0x12345678u && state.exiTransferControl == controls,
            "EXI transfer support changed ordinary RAM behavior.");
    state.memoryMap = PpcMemoryMap::Flat;
    ppc_write_u32(state, 4, 0x12345678u);
    Require(ppc_read_u32(state, 4) == 0x12345678u && state.exiTransferControl == controls,
            "EXI transfer support changed flat-memory behavior.");
}

void TestExiInitializationRegisters() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    const auto ram = state.memory;
    state.exiInitializationEnabled = true;
    for (unsigned channel = 0; channel < 3; ++channel) {
        const uint32_t address = 0xcc006800u + channel * 0x14u;
        const uint32_t supported = channel == 2 ? 0x7fu : channel == 0 ? 0x3c7fu : 0x1c7fu;
        const auto reject = [&](uint32_t value) {
            const auto before = state.exiStatus;
            bool rejected = false;
            try {
                ppc_write_u32(state, address, value);
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            Require(rejected && before == state.exiStatus && state.memory == ram,
                    "Unsupported EXI initialization write did not fail without mutation.");
        };
        for (uint32_t initial = 0; initial <= supported; ++initial) {
            if ((initial & ~supported) != 0) continue;
            for (uint32_t value : {0u, 0x2000u}) {
                if (value != 0 && channel != 0) continue;
                state.exiStatus[channel] = initial;
                state.exiTransferControl[channel] = 0x3eu;
                ppc_write_u32(state, address, value);
                const uint32_t expected = (initial & (channel == 2 ? 0xau : 0x180au)) | value;
                Require(ppc_read_u32(state, address) == expected &&
                            state.exiTransferControl[channel] == 0x3eu &&
                            state.memory == ram,
                        "EXI initialization reset damaged pending/presence state or failed to reset clock/enables.");
            }
        }
        state.exiStatus[channel] = supported;
        state.exiTransferControl[channel].reset();
        reject(0);
        for (uint32_t control : {1u, 0x40u, UINT32_MAX}) {
            state.exiTransferControl[channel] = control;
            reject(0);
        }
        state.exiTransferControl[channel] = 0;
        for (uint32_t value : {1u, 4u, 0x80u, 0x200u, 0x800u, UINT32_MAX}) reject(value);
        if (channel != 0) reject(0x2000u);
        for (unsigned bit = 0; bit < 32; ++bit) {
            if ((supported & (1u << bit)) != 0) continue;
            state.exiStatus[channel] = supported | (1u << bit);
            reject(0);
        }
    }
    state.exiStatus[0] = 0x3c7fu;
    ppc_write_u32(state, 0xcc006800u, 0x1070u);
    Require(state.exiStatus[0] == 0x187au,
            "Original interrupt masking did not clear ROMDIS while preserving pending/presence state.");
    state.exiInitializationEnabled = false;
    state.exiStatus[0] = 0x70u;
    bool rejected = false;
    try {
        ppc_write_u32(state, 0xcc006800u, 0);
    } catch (const std::runtime_error&) {
        rejected = true;
    }
    Require(rejected && state.exiStatus[0] == 0x70u && state.memory == ram,
            "EXI initialization reset did not require explicit opt-in.");
}

void TestExiStatus() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    const auto ram = state.memory;
    const auto rejectAccess = [&](auto operation) {
        const auto before = state.exiStatus;
        bool rejected = false;
        try {
            operation();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        Require(rejected && state.exiStatus == before && state.memory == ram,
                "Unsupported EXI access changed register state or RAM.");
    };
    for (unsigned channel = 0; channel < 3; ++channel) {
        const uint32_t address = 0xcc006800u + channel * 0x14u;
        state.gpr[4] = address;
        rejectAccess([&] { ppc_0000B200(state); });
        rejectAccess([&] { ppc_0000B210(state); });
        const uint32_t supported = channel == 2 ? 0x7fu : 0x1c7fu;
        const uint32_t command = channel == 2 ? 0x70u : 0x1070u;
        unsigned combinations = 0;
        for (uint32_t initial = 0; initial <= supported; ++initial) {
            if ((initial & ~supported) != 0) continue;
            ++combinations;
            state.exiStatus[channel] = initial;
            state.cr = 0x12345678u;
            state.xer = 0xe0000000u;
            ppc_0000B200(state);
            Require(state.gpr[3] == initial && state.pc == 0xb204u,
                    "Compiled EXI status readback differs.");
            for (unsigned repeat = 0; repeat < 2; ++repeat) {
                state.gpr[3] = initial & command;
                ppc_0000B210(state);
                ppc_0000B200(state);
                Require(state.gpr[3] == (initial & (channel == 2 ? 0x7au : 0x187au)) &&
                            state.cr == 0x12345678u && state.xer == 0xe0000000u,
                        "EXI masking changed status, presence, clock or CPU flags.");
            }
        }
        Require(combinations == (channel == 2 ? 128u : 1024u) && state.memory == ram,
                "EXI state coverage or RAM preservation differs.");
        const auto rejectWrite = [&](uint32_t previous, uint32_t value) {
            state.exiStatus[channel] = previous;
            const auto before = state.exiStatus;
            state.gpr[3] = value;
            state.pc = 0xb210u;
            bool rejected = false;
            try {
                ppc_0000B210(state);
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("only idle startup interrupt masking") != std::string::npos;
            }
            Require(rejected && state.exiStatus == before && state.memory == ram &&
                        state.gpr[3] == value && state.pc == 0xb210u,
                    "Unsupported EXI control write changed state.");
        };
        for (unsigned bit = 0; bit < 32; ++bit) {
            rejectWrite(supported, command ^ (1u << bit));
            if ((supported & (1u << bit)) == 0) {
                const uint32_t previous = supported | (1u << bit);
                rejectWrite(previous, previous & command);
            }
        }
        state.exiStatus[channel] = supported;
        for (uint32_t offset : {1u, 2u, 3u, 4u, 8u, 12u, 16u}) {
            state.gpr[4] = address + offset;
            rejectAccess([&] { ppc_0000B200(state); });
            rejectAccess([&] { ppc_0000B210(state); });
        }
        state.gpr[4] = address - 0xc0000000u;
        rejectAccess([&] { ppc_0000B200(state); });
        rejectAccess([&] { ppc_0000B210(state); });
        for (unsigned width : {1u, 2u, 8u}) {
            rejectAccess([&] {
                if (width == 1) (void)ppc_read_u8(state, address);
                else if (width == 2) (void)ppc_read_u16(state, address);
                else (void)ppc_read_u64(state, address);
            });
            rejectAccess([&] {
                if (width == 1) ppc_write_u8(state, address, 0);
                else if (width == 2) ppc_write_u16(state, address, 0);
                else ppc_write_u64(state, address, 0);
            });
        }
        state.exiStatus[channel].reset();
        state.gpr[4] = address;
        rejectAccess([&] { ppc_0000B200(state); });
    }
    state.exiStatus = {0x1c7fu, 0x1c7fu, 0x7fu};
    state.gpr[4] = 0xc0001000u;
    state.gpr[3] = 0x12345678u;
    ppc_0000B210(state);
    ppc_0000B200(state);
    Require(state.gpr[3] == 0x12345678u && state.exiStatus[0] == 0x1c7fu &&
                state.exiStatus[1] == 0x1c7fu && state.exiStatus[2] == 0x7fu,
            "EXI support changed RAM access or unrelated channels.");
    state.memoryMap = PpcMemoryMap::Flat;
    ppc_write_u32(state, 4, 0x12345678u);
    Require(ppc_read_u32(state, 4) == 0x12345678u && state.exiStatus[0] == 0x1c7fu,
            "EXI option changed flat-memory semantics.");
}

void TestAiControl() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    state.gpr[4] = 0xcc006c00u;
    const auto ram = state.memory;
    const auto rejectAccess = [&](auto operation) {
        const auto previous = state.aiControl;
        bool rejected = false;
        try {
            operation();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        Require(rejected && state.aiControl == previous && state.memory == ram,
                "Unsupported AI access mutated device state or RAM.");
    };
    rejectAccess([&] { ppc_0000B200(state); });
    rejectAccess([&] { ppc_0000B210(state); });
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 4;
    unsigned combinations = 0;
    for (uint32_t initial = 0; initial <= 0x5e; ++initial) {
        if ((initial & ~0x5eu) != 0) continue;
        ++combinations;
        state.aiControl = initial;
        ppc_0000B200(state);
        Require(state.gpr[3] == initial && state.pc == 0xb204u,
                "Compiled AI readback differs.");
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            state.gpr[3] = initial & 0x52u;
            ppc_0000B210(state);
            Require(state.aiControl == (initial & 0x5au) && state.pc == 0xb214u,
                    "AI masking changed pending status, validity or sample-rate bits.");
            ppc_0000B200(state);
            Require(state.gpr[3] == (initial & 0x5au) && state.memory == ram &&
                        state.cr == 0x12345678u && state.xer == 0xe0000000u && state.fpscr == 4,
                    "AI readback changed RAM or CPU status.");
        }
    }
    Require(combinations == 32, "AI initial-state coverage is incomplete.");
    const auto rejectWrite = [&](uint32_t previous, uint32_t value) {
        state.aiControl = previous;
        state.gpr[3] = value;
        state.pc = 0xb210u;
        bool rejected = false;
        try {
            ppc_0000B210(state);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("only idle startup interrupt masking") != std::string::npos;
        }
        Require(rejected && state.aiControl == previous && state.pc == 0xb210u &&
                    state.gpr[3] == value && state.memory == ram,
                "Unsupported AI command, acknowledgement or control change was accepted.");
    };
    for (unsigned bit = 0; bit < 32; ++bit) {
        rejectWrite(0x5e, 0x52u ^ (1u << bit));
        if ((~0x5eu & (1u << bit)) != 0) {
            const uint32_t initial = 0x5eu | (1u << bit);
            rejectWrite(initial, initial & ~0x2cu);
        }
    }
    state.aiControl = 0x5e;
    for (uint32_t address : {0xcc006bfcu, 0xcc006bffu, 0xcc006c01u, 0xcc006c02u,
                             0xcc006c03u, 0xcc006c04u, 0x0c006c00u, 0xcc006800u}) {
        state.gpr[4] = address;
        rejectAccess([&] { ppc_0000B200(state); });
        rejectAccess([&] { ppc_0000B210(state); });
    }
    for (unsigned width : {1u, 2u, 8u}) {
        rejectAccess([&] {
            if (width == 1) (void)ppc_read_u8(state, 0xcc006c00u);
            else if (width == 2) (void)ppc_read_u16(state, 0xcc006c00u);
            else (void)ppc_read_u64(state, 0xcc006c00u);
        });
        rejectAccess([&] {
            if (width == 1) ppc_write_u8(state, 0xcc006c00u, 0);
            else if (width == 2) ppc_write_u16(state, 0xcc006c00u, 0);
            else ppc_write_u64(state, 0xcc006c00u, 0);
        });
    }
    state.aiControl.reset();
    rejectAccess([&] { (void)ppc_read_u32(state, 0xcc006c00u); });
    state.aiControl = 0x5e;
    state.gpr[4] = 0xc0001000u;
    state.gpr[3] = 0x12345678u;
    ppc_0000B210(state);
    ppc_0000B200(state);
    Require(state.gpr[3] == 0x12345678u && state.aiControl == 0x5e,
            "AI support changed ordinary RAM access.");
    state.memoryMap = PpcMemoryMap::Flat;
    ppc_write_u32(state, 4, 0x12345678u);
    Require(ppc_read_u32(state, 4) == 0x12345678u && state.aiControl == 0x5e,
            "AI option changed flat-memory semantics.");
}

void TestDspControl() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    state.gpr[4] = 0xcc00500au;
    const auto ram = state.memory;
    const auto rejectAccess = [&](auto operation) {
        const auto previous = state.dspControl;
        bool rejected = false;
        try {
            operation();
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        Require(rejected && state.dspControl == previous && state.memory == ram,
                "Unsupported DSP access mutated device state or RAM.");
    };
    rejectAccess([&] { ppc_0000B220(state); });
    rejectAccess([&] { ppc_0000B230(state); });
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 4;
    for (unsigned initial = 0; initial <= 0x7fc; initial += 4) {
        state.dspControl = static_cast<uint16_t>(initial);
        ppc_0000B220(state);
        Require(state.gpr[3] == initial && state.pc == 0xb224u,
                "Compiled DSP readback differs.");
        for (unsigned repeat = 0; repeat < 2; ++repeat) {
            state.gpr[3] = 0xffff0000u | (initial & 0x604u);
            ppc_0000B230(state);
            Require(state.dspControl == (initial & 0x6acu) && state.pc == 0xb234u,
                    "DSP startup masking changed pending status or unrelated control bits.");
            ppc_0000B220(state);
            Require(state.gpr[3] == (initial & 0x6acu) &&
                        state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                        state.fpscr == 4 && state.memory == ram,
                    "DSP readback or masking changed RAM or CPU status.");
        }
    }
    const auto rejectWrite = [&](uint16_t previous, uint16_t value) {
        state.dspControl = previous;
        state.gpr[3] = value;
        state.pc = 0xb230u;
        bool rejected = false;
        try {
            ppc_0000B230(state);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("only startup interrupt masking") != std::string::npos;
        }
        Require(rejected && state.dspControl == previous && state.pc == 0xb230u &&
                    state.gpr[3] == value && state.memory == ram,
                "Unsupported DSP command, acknowledgement or control change was accepted.");
    };
    for (unsigned bit = 0; bit < 16; ++bit) {
        rejectWrite(0x7fc, static_cast<uint16_t>(0x604u ^ (1u << bit)));
        if ((0xf803u & (1u << bit)) != 0) {
            const auto initial = static_cast<uint16_t>(4u | (1u << bit));
            rejectWrite(initial, initial);
        }
    }
    state.dspControl = 0x7fc;
    for (uint32_t address : {0xcc005008u, 0xcc005009u, 0xcc00500bu, 0xcc00500cu,
                             0x0c00500au, 0xcc006c00u}) {
        state.gpr[4] = address;
        rejectAccess([&] { ppc_0000B220(state); });
        rejectAccess([&] { ppc_0000B230(state); });
    }
    for (unsigned width : {1u, 4u, 8u}) {
        rejectAccess([&] {
            if (width == 1) (void)ppc_read_u8(state, 0xcc00500au);
            else if (width == 4) (void)ppc_read_u32(state, 0xcc00500au);
            else (void)ppc_read_u64(state, 0xcc00500au);
        });
        rejectAccess([&] {
            if (width == 1) ppc_write_u8(state, 0xcc00500au, 0);
            else if (width == 4) ppc_write_u32(state, 0xcc00500au, 0);
            else ppc_write_u64(state, 0xcc00500au, 0);
        });
    }
    state.dspControl.reset();
    rejectAccess([&] { (void)ppc_read_u16(state, 0xcc00500au); });
    state.dspControl = 0x7fc;
    state.gpr[4] = 0xc0001000u;
    state.gpr[3] = 0x1234;
    ppc_0000B230(state);
    ppc_0000B220(state);
    Require(state.gpr[3] == 0x1234 && state.dspControl == 0x7fc,
            "DSP control support changed RAM halfword access.");
    state.memoryMap = PpcMemoryMap::Flat;
    state.dspControl = 0x7fc;
    ppc_write_u16(state, 4, 0x1234);
    Require(ppc_read_u16(state, 4) == 0x1234 && state.dspControl == 0x7fc,
            "DSP option changed flat-memory semantics.");
}

void TestMiInterruptMask() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    state.gpr[4] = 0xcc00401cu;
    const auto ram = state.memory;
    for (auto operation : {ppc_0000B220, ppc_0000B230}) {
        bool rejected = false;
        try {
            operation(state);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        Require(rejected && !state.miInterruptMask.has_value() && state.memory == ram,
                "MI register was enabled implicitly.");
    }
    state.miInterruptMask = 0x1fu;
    ppc_0000B220(state);
    Require(state.gpr[3] == 0x1fu && state.pc == 0xb224u,
            "Compiled MI read did not return the supplied mask.");
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 4;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        state.gpr[3] = 0xffff0000u; // sth transfers only the low halfword.
        ppc_0000B230(state);
        Require(state.miInterruptMask == 0 && state.pc == 0xb234u,
                "Compiled MI reset failed or ignored halfword truncation.");
        state.gpr[3] = UINT32_MAX;
        ppc_0000B220(state);
        Require(state.gpr[3] == 0 && state.memory == ram &&
                    state.cr == 0x12345678u && state.xer == 0xe0000000u && state.fpscr == 4,
                "MI readback changed RAM or CPU status.");
    }
    for (unsigned bit = 0; bit < 16; ++bit) {
        state.gpr[3] = 1u << bit;
        state.pc = 0xb230u;
        bool rejected = false;
        try {
            ppc_0000B230(state);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("only zero reset") != std::string::npos;
        }
        Require(rejected && state.miInterruptMask == 0 && state.memory == ram &&
                    state.pc == 0xb230u && state.gpr[3] == (1u << bit),
                "Unsupported MI mask mutated device state, RAM, PC or source.");
    }
    for (uint32_t address : {0xcc00401bu, 0xcc00401du, 0xcc00401eu,
                              0x0c00401cu, 0xcc00500au}) {
        state.gpr[4] = address;
        for (auto operation : {ppc_0000B220, ppc_0000B230}) {
            bool rejected = false;
            try {
                operation(state);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            Require(rejected && state.miInterruptMask == 0 && state.memory == ram,
                    "Neighboring, unaligned or unsupported MI/device address was accepted.");
        }
    }
    for (unsigned width : {1u, 4u, 8u}) {
        for (bool write : {false, true}) {
            bool rejected = false;
            try {
                if (write) {
                    if (width == 1) ppc_write_u8(state, 0xcc00401cu, 0);
                    else if (width == 4) ppc_write_u32(state, 0xcc00401cu, 0);
                    else ppc_write_u64(state, 0xcc00401cu, 0);
                } else {
                    if (width == 1) (void)ppc_read_u8(state, 0xcc00401cu);
                    else if (width == 4) (void)ppc_read_u32(state, 0xcc00401cu);
                    else (void)ppc_read_u64(state, 0xcc00401cu);
                }
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            Require(rejected && state.miInterruptMask == 0 && state.memory == ram,
                    "Unsupported MI access width bypassed register checks.");
        }
    }
    state.gpr[4] = 0xc0001000u;
    state.gpr[3] = 0x1234u;
    ppc_0000B230(state);
    ppc_0000B220(state);
    Require(state.gpr[3] == 0x1234u && state.miInterruptMask == 0,
            "MI support changed ordinary halfword RAM access.");
    state.miInterruptMask.reset();
    bool rejected = false;
    try {
        (void)ppc_read_u16(state, 0xcc00401cu);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    Require(rejected, "Disabling MI support left a readable register.");
    state.memoryMap = PpcMemoryMap::Flat;
    state.miInterruptMask = 0x1fu;
    ppc_write_u16(state, 4, 0x1234u);
    Require(ppc_read_u16(state, 4) == 0x1234u && state.miInterruptMask == 0x1fu,
            "MI option changed flat-memory semantics.");
}

void TestPiInterruptMask() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    state.gpr[4] = 0xcc003004u;
    state.gpr[3] = 0xf0u;
    const auto ram = state.memory;
    bool rejected = false;
    try {
        ppc_0000B210(state);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    Require(rejected && !state.piInterruptMask.has_value() && state.memory == ram,
            "PI register was enabled implicitly.");
    state.piInterruptMask = 0;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 4;
    for (unsigned repeat = 0; repeat < 2; ++repeat) {
        state.gpr[3] = 0xf0u;
        ppc_0000B210(state);
        Require(*state.piInterruptMask == 0xf0u && state.pc == 0xb214u,
                "Compiled PI startup write did not persist.");
        state.gpr[3] = 0;
        ppc_0000B200(state);
        Require(state.gpr[3] == 0xf0u && state.pc == 0xb204u &&
                    state.memory == ram && state.cr == 0x12345678u &&
                    state.xer == 0xe0000000u && state.fpscr == 4,
                "Compiled PI readback changed RAM or CPU status.");
    }
    for (uint32_t value : {0u, 1u, 0xefu, 0xf1u, UINT32_MAX}) {
        state.gpr[3] = value;
        rejected = false;
        try {
            ppc_0000B210(state);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("only startup mask") != std::string::npos;
        }
        Require(rejected && *state.piInterruptMask == 0xf0u && state.memory == ram,
                "Unsupported PI mask value mutated device state or RAM.");
    }
    for (uint32_t address : {0xcc003000u, 0xcc003005u, 0xcc003008u,
                              0x0c003004u, 0xcc00401cu}) {
        state.gpr[4] = address;
        for (auto operation : {ppc_0000B200, ppc_0000B210}) {
            rejected = false;
            try {
                operation(state);
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            Require(rejected && *state.piInterruptMask == 0xf0u && state.memory == ram,
                    "Unknown or unaligned PI/device address was accepted.");
        }
    }
    for (unsigned width : {1u, 2u, 8u}) {
        for (bool write : {false, true}) {
            rejected = false;
            try {
                if (write) {
                    if (width == 1) ppc_write_u8(state, 0xcc003004u, 0xf0);
                    else if (width == 2) ppc_write_u16(state, 0xcc003004u, 0xf0);
                    else ppc_write_u64(state, 0xcc003004u, 0xf0);
                } else {
                    if (width == 1) (void)ppc_read_u8(state, 0xcc003004u);
                    else if (width == 2) (void)ppc_read_u16(state, 0xcc003004u);
                    else (void)ppc_read_u64(state, 0xcc003004u);
                }
            } catch (const std::out_of_range&) {
                rejected = true;
            }
            Require(rejected && *state.piInterruptMask == 0xf0u && state.memory == ram,
                    "Unsupported PI access width bypassed register checks.");
        }
    }
    state.gpr[4] = 0x80001000u;
    state.gpr[3] = 0x12345678u;
    ppc_0000B210(state);
    ppc_0000B200(state);
    Require(state.gpr[3] == 0x12345678u && *state.piInterruptMask == 0xf0u,
            "PI register support changed ordinary RAM access.");
    state.piInterruptMask.reset();
    rejected = false;
    try {
        (void)ppc_read_u32(state, 0xcc003004u);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    Require(rejected, "Disabling PI support left a readable register.");
    state.memoryMap = PpcMemoryMap::Flat;
    state.memory.resize(64);
    state.piInterruptMask = 0xf0u;
    ppc_write_u32(state, 4, 0x11223344u);
    Require(ppc_read_u32(state, 4) == 0x11223344u && *state.piInterruptMask == 0xf0u,
            "PI option changed flat-memory semantics.");
}

void TestCacheControlRegisters() {
    PpcState state;
    state.memory.resize(64, 0xa5);
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    state.fpscr = 0xf1234567u;
    state.msr = 0x2030u;
    state.hid2 = 0xa0000000u;
    state.gqr.fill(0x12345678u);
    state.fpr.fill({UINT64_MAX, UINT64_MAX});
    const auto memory = state.memory;
    const auto fpr = state.fpr;
    const auto gqr = state.gqr;
    for (uint32_t preserved : {0u, 0x00100000u}) {
        for (uint32_t initial : {0u, 0x200u, 0x4000u, 0x4200u,
                                 0x8000u, 0x8200u, 0xc000u, 0xc200u}) {
            state.hid0 = initial | preserved;
            ppc_00009340(state);
            Require(state.gpr[3] == (initial | preserved) && state.pc == 0x9344u,
                    "Compiled HID0 read did not preserve cache/other state.");
            for (unsigned combination = 0; combination < 16; ++combination) {
                const uint32_t desired = ((combination & 3u) << 14) |
                                         ((combination & 4u) << 9) |
                                         ((combination & 8u) << 6) | preserved;
                for (unsigned repeat = 0; repeat < 2; ++repeat) {
                    state.gpr[3] = desired;
                    ppc_00009350(state);
                    Require(state.pc == 0x9354u && state.gpr[3] == desired &&
                                state.hid0 == (desired & ~0x800u),
                            "Compiled HID0 write failed enables, SPD, ICFI completion or repetition.");
                    ppc_00009340(state);
                    Require(state.gpr[3] == (desired & ~0x800u),
                            "Instruction-cache invalidation did not self-clear on readback.");
                }
            }
        }
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        const uint32_t flag = 1u << bit;
        if ((flag & 0xca00u) != 0) continue;
        for (bool clearing : {false, true}) {
            state.hid0 = 0xc000u | (clearing ? flag : 0u);
            const uint32_t before = state.hid0;
            state.gpr[3] = 0x800u | (clearing ? 0u : flag);
            bool rejected = false;
            try {
                ppc_00009350(state);
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("HID0") != std::string::npos;
            }
            Require(rejected && state.hid0 == before,
                    "Unsupported HID0 change mutated cache enables before rejection.");
        }
    }
    for (bool interrupts : {false, true}) {
        state.externalInterruptsEnabled = interrupts;
        const auto hid0 = state.hid0;
        const auto registers = std::vector<uint32_t>(std::begin(state.gpr), std::end(state.gpr));
        ppc_00009360(state);
        Require(state.pc == 0x9364u, "Compiled sync did not advance to the next instruction.");
        ppc_00009370(state);
        Require(state.pc == 0x9374u && state.hid0 == hid0 &&
                    state.externalInterruptsEnabled == interrupts &&
                    std::equal(registers.begin(), registers.end(), std::begin(state.gpr)),
                "Compiled barriers changed guest control/register state.");
    }
    Require(state.memory == memory && state.fpr == fpr && state.gqr == gqr &&
                state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                state.fpscr == 0xf1234567u && state.msr == 0x2030u &&
                state.hid2 == 0xa0000000u,
            "Cache controls or barriers changed unrelated CPU state or RAM.");
}

void TestRegisterAddCarry() {
    for (unsigned index = 0; index < 12; ++index) {
        const bool extended = index >= 6;
        const bool record = (index % 6) >= 3;
        const unsigned destination = index % 3 == 0 ? 3 : index % 3 == 1 ? 4 : 5;
        for (uint32_t left : {0u, 1u, 0x7fffffffu, 0x80000000u, UINT32_MAX}) {
            for (uint32_t right : {0u, 1u, 0x7fffffffu, 0x80000000u, UINT32_MAX}) {
                for (uint32_t xer : {0u, 0x20000000u, 0xc000007fu, 0xe000007fu}) {
                    PpcState state;
                    state.gpr[4] = left;
                    state.gpr[5] = right;
                    state.xer = xer;
                    state.cr = 0x12345678u;
                    const uint64_t sum = uint64_t{left} + right +
                        (extended ? ((xer >> 29) & 1u) : 0u);
                    const uint32_t result = static_cast<uint32_t>(sum);
                    const uint32_t flags = (result == 0 ? 2u :
                        (result & 0x80000000u) != 0 ? 8u : 4u) | (xer >> 31);
                    AddCarryTestFunctions[index](state);
                    Require(state.gpr[destination] == result &&
                                state.xer == ((xer & ~0x20000000u) |
                                    (sum > UINT32_MAX ? 0x20000000u : 0u)) &&
                                state.cr == (record ? (0x02345678u | (flags << 28)) :
                                                       0x12345678u) &&
                                state.pc == 0x9404u + index * 16u &&
                                (destination == 4 || state.gpr[4] == left) &&
                                (destination == 5 || state.gpr[5] == right),
                            "Compiled addc/adde lost carry, alias, CR0 or XER preservation.");
                }
            }
        }
    }
}

void TestL2ControlRegisters() {
    PpcState state;
    state.memory.resize(64, 0xa5);
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    const auto memory = state.memory;
    for (uint32_t preserved : {0u, 0x10000000u}) {
        state.l2cr = preserved | 0x80000000u;
        for (uint32_t desired : {0u, 0x00200000u, 0x00200000u, 0u, 0x80000000u}) {
            state.gpr[3] = desired | preserved;
            ppc_00009390(state);
            Require(state.l2cr == (desired | preserved) && state.pc == 0x9394u,
                    "L2CR write lost configuration or invalidation request.");
            ppc_00009380(state);
            Require(state.gpr[3] == (desired | preserved) &&
                        (state.gpr[3] & 1u) == 0 && state.pc == 0x9384u,
                    "L2CR read did not report synchronous invalidation completion.");
        }
    }
    for (unsigned bit = 0; bit < 32; ++bit) {
        const uint32_t flag = 1u << bit;
        if ((flag & 0x80200000u) != 0) continue;
        for (bool clearing : {false, true}) {
            state.l2cr = clearing ? flag : 0u;
            const uint32_t before = state.l2cr;
            state.gpr[3] = 0x00200000u | (clearing ? 0u : flag);
            bool rejected = false;
            try {
                ppc_00009390(state);
            } catch (const std::runtime_error& error) {
                rejected = std::string(error.what()).find("L2CR") != std::string::npos;
            }
            Require(rejected && state.l2cr == before,
                    "Unsupported L2CR configuration changed before rejection.");
        }
    }
    for (uint32_t initial : {0u, 0x00200000u, 0x80000000u}) {
        state.l2cr = initial;
        state.gpr[3] = 0x80200000u;
        bool rejected = false;
        try {
            ppc_00009390(state);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("disabled") != std::string::npos;
        }
        Require(rejected && state.l2cr == initial,
                "L2 invalidation while enabled was not rejected atomically.");
    }
    Require(state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                state.memory == memory,
            "L2 control transfer changed flags or guest RAM.");
}

void TestIndexedPairedAddresses() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    ppc_write_u32(state, 0x100u, 0x3f800000u);
    ppc_write_u32(state, 0x104u, 0xbf800000u);
    const PpcFpr expected = {UINT64_C(0x3ff0000000000000),
                             UINT64_C(0xbff0000000000000)};
    const auto memory = state.memory;
    state.gpr[0] = 0xcc000000u;
    state.gpr[4] = 0x80000100u;
    ppc_00009200(state);
    Require(state.fpr[3] == expected && state.gpr[0] == 0xcc000000u &&
                state.gpr[4] == 0x80000100u,
            "Indexed paired RA=0 incorrectly used GPR0 or updated the index.");
    for (uint32_t base : {0x80u, 0x80000080u, 0xc0000080u}) {
        state.gpr[3] = base;
        ppc_00009210(state);
        Require(state.fpr[3] == expected && state.gpr[3] == base + base,
                "Indexed paired update with RA=RB did not read both original base values.");
    }
    for (const auto& address : {std::pair<uint32_t, uint32_t>{0xf0u, 0x10u},
                               {0xfffffff0u, 0x110u}, {0x80000120u, 0xffffffe0u}}) {
        state.gpr[3] = address.first;
        state.gpr[4] = address.second;
        IndexedPairedLoadTestFunctions[1](state);
        Require(state.fpr[3] == expected &&
                    state.gpr[3] == address.first + address.second &&
                    state.gpr[4] == address.second,
                "Indexed paired load did not use 32-bit wrapped effective-address arithmetic.");
    }
    state.gpr[3] = 0xf0u;
    state.gpr[0] = 0x10u;
    ppc_00009220(state);
    Require(state.fpr[3] == expected && state.gpr[3] == 0xf0u && state.gpr[0] == 0x10u,
            "Indexed paired RB=0 did not read the actual GPR0 index.");
    Require(state.memory == memory, "Indexed paired load wrote guest RAM.");
}

void TestCarryAndSpecialRegisters() {
    for (uint32_t source : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (bool subtract : {false, true}) {
            PpcState state;
            state.gpr[4] = source;
            state.xer = 0xc0000000u | (subtract ? 0u : 0x20000000u);
            const uint64_t sum = static_cast<uint64_t>(source) +
                                 (subtract ? 0xffffffffu : 1u);
            if (subtract) {
                ppc_00003010(state);
            } else {
                ppc_00003000(state);
            }
            Require(state.gpr[3] == static_cast<uint32_t>(sum) &&
                        ((state.xer >> 29) & 1u) == (sum >> 32) &&
                        (state.xer & 0xc0000000u) == 0xc0000000u,
                    "Compiled addic. produced incorrect carry or changed overflow flags.");
            const uint32_t result = static_cast<uint32_t>(sum);
            Require((state.cr >> 28) ==
                        (result == 0 ? 3u : result < 0x80000000u ? 5u : 9u),
                    "Compiled addic. produced incorrect comparison flags.");
        }
    }
    PpcState state;
    state.gpr[3] = 0xf1234567u;
    ppc_00003020(state);
    Require(state.ctr == state.gpr[3] && state.gpr[4] == state.ctr &&
                state.cr == 0, "CTR register transfer failed.");
    ppc_00003030(state);
    Require(state.xer == state.gpr[3] && state.gpr[4] == state.xer &&
                state.cr == 0, "XER register transfer failed.");
}

void TestLocalIndirectBranches() {
    for (uint32_t target : {0x4088u, 0x4090u, 0x4093u, 0x5003u}) {
        PpcState state;
        state.ctr = target;
        state.lr = 0x6003u;
        ppc_00004080(state);
        Require(state.gpr[3] == (target == 0x4088 ? 99u : target < 0x5000 ? 7u : 1u) &&
                    state.pc == (target < 0x5000 ? 0x6000u : 0x5000u) &&
                    state.ctr == target && state.lr == 0x6003u,
                "Local CTR jump table, alignment or external tail dispatch failed.");
    }
    for (bool taken : {false, true}) {
        for (uint32_t target : {0x40acu, 0x5000u}) {
            PpcState state;
            state.ctr = target;
            state.lr = 0x6000u;
            state.cr = taken ? 0x20000000u : 0;
            ppc_000040A0(state);
            Require(state.gpr[3] == (!taken ? 3u : target == 0x40ac ? 9u : 0u) &&
                        state.pc == (taken && target == 0x5000 ? 0x5000u : 0x6000u) &&
                        state.ctr == target && state.cr == (taken ? 0x20000000u : 0u),
                    "Conditional local/external CTR branch changed condition/fallthrough behavior.");
        }
    }
    PpcState state;
    state.ctr = 0x40e0u;
    state.lr = 0x6000u;
    ppc_000040E0(state);
    Require(state.gpr[3] == 3 && state.pc == 0x6000u && state.ctr == 0x40e0u,
            "Backward local CTR branch did not retain native loop semantics.");
}

void TestLinkedBranchAndMultiLoad() {
    for (bool taken : {false, true}) {
        PpcState state;
        state.cr = taken ? 0x20000000u : 0;
        state.gpr[3] = 7;
        unsigned calls = 0;
        state.callGuest = [&calls](uint32_t address, PpcState& guest) {
            Require(address == 0x3050 && guest.lr == 0x3044,
                    "Conditional link branch used incorrect target/LR.");
            ++calls;
            guest.gpr[3] = 41;
        };
        ppc_00003040(state);
        Require(calls == static_cast<unsigned>(taken) &&
                    state.gpr[3] == (taken ? 42u : 8u) &&
                    state.pc == 0x3048 && state.lr == 0x3044,
                "Conditional call did not return to its continuation.");
    }
    PpcState state;
    state.memory.resize(128);
    for (uint32_t reg = 3; reg < 32; ++reg) {
        ppc_write_u32(state, (reg - 3) * 4, 0x1000u + reg);
    }
    state.gpr[4] = 0;
    ppc_00003050(state);
    for (uint32_t reg = 3; reg < 32; ++reg) {
        Require(state.gpr[reg] == 0x1000u + reg,
                "lmw changed its effective address while loading its base register.");
    }
}

void TestComparisonAndNor() {
    for (uint32_t left : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (uint32_t right : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
            PpcState state;
            state.gpr[3] = left;
            state.gpr[4] = right;
            state.xer = 0x80000000u;
            state.cr = 0x01234567u;
            ppc_00003070(state);
            const int64_t signedLeft = left < 0x80000000u ? int64_t{left}
                                                         : int64_t{left} - 0x100000000ll;
            const int64_t signedRight = right < 0x80000000u ? int64_t{right}
                                                           : int64_t{right} - 0x100000000ll;
            const uint32_t signedField = signedLeft < signedRight ? 9u :
                                         signedLeft > signedRight ? 5u : 3u;
            Require(state.cr == ((signedField << 28) | 0x01234567u),
                    "cmpw did not compare signed registers or preserve other CR fields.");
            state.cr = 0x12345678u;
            ppc_00003080(state);
            const uint32_t unsignedField = left < right ? 9u : left > right ? 5u : 3u;
            Require(state.cr == ((0x12345678u & ~0x000f0000u) | (unsignedField << 16)),
                    "cmplw did not compare unsigned registers in the selected CR field.");
            state.gpr[5] = right;
            ppc_00003060(state);
            const uint32_t expected = ~(left | right);
            Require(state.gpr[4] == expected && state.gpr[3] == left &&
                        state.gpr[5] == right &&
                        (state.cr >> 28) ==
                            (expected == 0 ? 3u : expected < 0x80000000u ? 5u : 9u),
                    "nor. produced an incorrect result, register write or CR0.");
        }
    }
}

template <typename Function>
void Reject(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(message) != std::string::npos,
                "Unexpected native-dispatch rejection.");
        return;
    }
    throw std::runtime_error("Expected native-dispatch failure: " + message);
}

void TestCountLeadingZeros() {
    for (uint32_t bit = 0; bit <= 32; ++bit) {
        PpcState state;
        state.gpr[3] = bit == 32 ? 0u : 0x80000000u >> bit;
        state.gpr[4] = 0xdeadbeefu;
        state.cr = 0x01234567u;
        state.xer = 0xa0000000u;
        ppc_00003090(state);
        Require(state.gpr[4] == bit && state.xer == 0xa0000000u &&
                    state.cr == ((bit == 0 ? 3u : 5u) << 28 | 0x01234567u),
                "cntlzw. count, destination, CR0 or XER is incorrect.");
        state.cr = 0x12345678u;
        ppc_000030A0(state);
        Require(state.gpr[3] == bit && state.cr == 0x12345678u,
                "Non-record cntlzw changed CR or failed with identical source/destination.");
    }
}

void TestSignExtendHalfword() {
    for (uint32_t value : {0u, 1u, 0x7fffu, 0x8000u, 0xffffu,
                           0x80000000u, 0xffff7fffu, 0x12348001u}) {
        PpcState state;
        state.gpr[3] = value;
        state.gpr[4] = 0xdeadbeefu;
        state.xer = 0xe0000000u;
        state.cr = 0x01234567u;
        const uint32_t low = value & 0xffffu;
        const uint32_t expected = low >= 0x8000u ? 0xffff0000u | low : low;
        ppc_000030B0(state);
        Require(state.gpr[4] == expected && state.gpr[3] == value &&
                    state.xer == 0xe0000000u &&
                    state.cr == (((expected == 0 ? 3u :
                                  expected < 0x80000000u ? 5u : 9u) << 28) | 0x01234567u),
                "extsh. result, registers, CR0 or XER mismatch.");
        state.cr = 0x87654321u;
        ppc_000030C0(state);
        Require(state.gpr[3] == expected && state.cr == 0x87654321u,
                "Non-record extsh changed CR or failed with identical source/destination.");
    }
}

void TestSignExtendByteAndNegate() {
    for (uint32_t byte = 0; byte < 256; ++byte) {
        PpcState state;
        const uint32_t value = 0x12345600u | byte;
        const uint32_t expected = byte < 128 ? byte : 0xffffff00u | byte;
        state.gpr[3] = value;
        state.cr = 0x01234567u;
        state.xer = 0xe0000000u;
        ppc_00003180(state);
        Require(state.gpr[4] == expected && state.gpr[3] == value &&
                    state.xer == 0xe0000000u &&
                    state.cr == (((byte == 0 ? 3u : byte < 128 ? 5u : 9u) << 28) |
                                 0x01234567u),
                "Compiled extsb. result, CR0 or preserved registers mismatch.");
        const auto cr = state.cr;
        ppc_00003190(state);
        Require(state.gpr[3] == expected && state.cr == cr,
                "Non-record aliased extsb changed CR or used the wrong source.");
    }
    for (uint32_t value : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        PpcState state;
        state.gpr[4] = value;
        state.cr = 0x01234567u;
        state.xer = 0xe0000000u;
        const uint32_t expected = 0u - value;
        ppc_000031A0(state);
        Require(state.gpr[3] == expected && state.gpr[4] == value &&
                    state.xer == 0xe0000000u &&
                    state.cr == (((expected == 0 ? 3u : expected < 0x80000000u ? 5u : 9u) << 28) |
                                 0x01234567u),
                "Compiled neg. result, CR0 or preserved registers mismatch.");
        const auto cr = state.cr;
        ppc_000031B0(state);
        Require(state.gpr[4] == expected && state.cr == cr && state.xer == 0xe0000000u,
                "Non-record aliased neg changed CR/XER or used the wrong source.");
    }
}

void TestTimeBaseAndConditionXor() {
    PpcState state;
    Reject([&] { ppc_000030D0(state); }, "No native PowerPC time-base source");
    unsigned reads = 0;
    state.cr = 0x87654321u;
    state.xer = 0xe0000000u;
    for (uint64_t ticks : {0ull, 0xffffffffull, 0x100000000ull,
                            0x12345678abcdef01ull, UINT64_MAX}) {
        state.readTimeBase = [&reads, ticks] { ++reads; return ticks; };
        ppc_000030D0(state);
        ppc_000030E0(state);
        Require(state.gpr[3] == static_cast<uint32_t>(ticks) &&
                    state.gpr[4] == static_cast<uint32_t>(ticks >> 32) &&
                    state.cr == 0x87654321u && state.xer == 0xe0000000u,
                "mftb low/high reads, rollover or CR/XER preservation failed.");
    }
    Require(reads == 10, "mftb did not perform exactly one native clock read per instruction.");
    for (uint32_t cr : {0u, 0xffffffffu, 0x01000000u, 0x00800000u, 0x87654321u}) {
        state.cr = cr;
        ppc_000030F0(state);
        const auto expected = (cr & ~(1u << 25)) |
                              ((((cr >> 24) ^ (cr >> 23)) & 1u) << 25);
        Require(state.cr == expected && state.pc == 0x30f4u,
                "crxor changed the wrong condition bit or did not advance PC.");
        ppc_00003100(state);
        Require(state.cr == (expected & ~(1u << 25)) && state.pc == 0x3104u,
                "crclr did not clear exactly CR bit 6 or advance PC.");
    }
    Require(GameCubeTicksFromNanoseconds(0) == 0 &&
                GameCubeTicksFromNanoseconds(1000000000) == 40500000 &&
                GameCubeTicksFromNanoseconds(1500000000) == 60750000 &&
                GameCubeTicksFromNanoseconds(100000000001ll) == 4050000000ull &&
                GameCubeTicksFromNanoseconds(1000000000000ll) == 40500000000ull &&
                GameCubeTicksFromNanoseconds(INT64_MAX) == 373546567492618420ull,
            "Native time-base conversion has incorrect rate, truncation or overflow.");
    Reject([&] { GameCubeTicksFromNanoseconds(-1); }, "backwards");
    state.readTimeBase = MakeNativeTimeBase();
    const auto first = ppc_read_time_base(state);
    uint64_t last = first;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (last == first && std::chrono::steady_clock::now() < deadline) {
        last = ppc_read_time_base(state);
    }
    Require(last > first, "Native monotonic clock did not advance.");
}

void TestSubtractImmediateCarry() {
    for (uint32_t source : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (uint32_t immediate : {0u, 1u, 0xffffffffu}) {
            PpcState state;
            state.gpr[4] = source;
            state.cr = 0x87654321u;
            state.xer = 0xc0000000u | (immediate == 0 ? 0x20000000u : 0u);
            if (immediate == 0) {
                ppc_00003130(state);
            } else if (immediate == 1) {
                ppc_00003110(state);
            } else {
                ppc_00003120(state);
            }
            Require(state.gpr[3] == immediate - source && state.gpr[4] == source &&
                        state.cr == 0x87654321u &&
                        state.xer == (0xc0000000u |
                                      (immediate >= source ? 0x20000000u : 0u)),
                    "subfic result, carry, input register or CR/overflow preservation failed.");
        }
    }
}

void TestSubtractExtended() {
    for (uint32_t left : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu}) {
        for (uint32_t right : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu}) {
            for (uint32_t carry : {0u, 1u}) {
                for (uint32_t flags : {0u, 0xc0000001u}) {
                    for (unsigned variant = 0; variant < 4; ++variant) {
                        PpcState state;
                        state.gpr[4] = left;
                        state.gpr[5] = right;
                        state.xer = flags | (carry << 29);
                        state.cr = 0x12345678u;
                        const uint32_t result = right - left - (1u - carry);
                        const bool noBorrow = uint64_t{right} >= uint64_t{left} + 1 - carry;
                        if (variant == 0) ppc_000031C0(state);
                        if (variant == 1) ppc_000031D0(state);
                        if (variant == 2) ppc_000031E0(state);
                        if (variant == 3) ppc_000031F0(state);
                        const uint32_t destination = variant == 2 ? 4u : variant == 3 ? 5u : 3u;
                        const bool record = variant == 1 || variant == 2;
                        const uint32_t comparison = result == 0 ? 2u :
                                                    result < 0x80000000u ? 4u : 8u;
                        const uint32_t cr = ((comparison | (flags >> 31)) << 28) | 0x02345678u;
                        Require(state.gpr[destination] == result &&
                                    (destination == 4 || state.gpr[4] == left) &&
                                    (destination == 5 || state.gpr[5] == right) &&
                                    state.xer == (flags | (noBorrow ? 0x20000000u : 0u)) &&
                                    state.cr == (record ? cr : 0x12345678u),
                                "Compiled subfe result, carry/borrow, CR0, flags or aliasing mismatch.");
                    }
                }
            }
        }
    }
}

void TestMultiplyDivideWords() {
    for (uint32_t left : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (uint32_t right : {0u, 1u, 2u, 31u, 0x80000000u, 0xffffffffu}) {
            PpcState state;
            state.gpr[4] = left;
            state.gpr[5] = right;
            state.cr = 0x01234567u;
            state.xer = 0xe0000000u;
            const uint32_t product = static_cast<uint32_t>(uint64_t{left} * right);
            ppc_00003140(state);
            const auto check = [&](uint32_t expected) {
                Require(state.gpr[3] == expected && state.gpr[4] == left &&
                            state.gpr[5] == right && state.xer == 0xe0000000u &&
                            state.cr == (((expected == 0 ? 3u :
                                          expected < 0x80000000u ? 5u : 9u) << 28) |
                                         0x01234567u),
                        "Compiled mullw./divwu. result, CR0 or preserved registers mismatch.");
            };
            check(product);
            const auto productCr = state.cr;
            ppc_00003160(state);
            Require(state.gpr[3] == product && state.cr == productCr,
                    "Non-record mullw changed CR or produced a different product.");
            if (right == 0) {
                const auto previous = state.gpr[3];
                const auto cr = state.cr;
                Reject([&] { ppc_00003150(state); }, "zero divisor");
                Reject([&] { ppc_00003170(state); }, "zero divisor");
                Require(state.gpr[3] == previous && state.cr == cr,
                        "Undefined division partially modified destination or CR.");
            } else {
                ppc_00003150(state);
                check(left / right);
                const auto quotientCr = state.cr;
                ppc_00003170(state);
                Require(state.gpr[3] == left / right && state.cr == quotientCr,
                        "Non-record divwu changed CR or produced a different quotient.");
            }
        }
    }
}

std::function<void(PpcState&)> nativeTestAction;

void NativeTestHook(PpcState& state) {
    nativeTestAction(state);
}

void TestNativeDispatch() {
    PpcState state;
    state.memory.resize(0x5000);
    const std::vector<NativeTranslation> root = {
        {0x4000, ppc_00004000, {0x7fe802a6u, 0x4800001du, 0x38630001u,
                               0x7fe803a6u, 0x4e800020u}},
        {0x4060, ppc_00004060, {0x7fe802a6u, 0x4e800421u,
                               0x7fe803a6u, 0x4e800020u}}};
    const std::vector<NativeTranslation> module = {
        {0x4020, ppc_00004020, {0x38630002u, 0x4800001cu}},
        {0x4040, ppc_00004040, {0x38630004u, 0x4e800020u}}};
    for (const auto* bindings : {&root, &module}) {
        for (const auto& binding : *bindings) {
            for (size_t i = 0; i < binding.expectedWords.size(); ++i) {
                ppc_write_u32(state, binding.address + static_cast<uint32_t>(i * 4),
                              binding.expectedWords[i]);
            }
        }
    }
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, root);
    dispatcher.Register(1, state, module);
    unsigned previousCalls = 0;
    state.callGuest = [&previousCalls](uint32_t, PpcState&) { ++previousCalls; };
    for (const uint32_t entry : {0x4000u, 0x4060u}) {
        state.gpr[3] = 0xc800u;
        ppc_00009350(state);
        ppc_00009360(state);
        ppc_00009370(state);
        Require(dispatcher.Contains(0x4020) && state.hid0 == 0xc000u,
                "Cache invalidation removed a valid static native translation.");
        state.gpr[3] = 0x4020;
        ppc_0000B100(state);
        ppc_0000B120(state);
        Require(dispatcher.Contains(0x4020),
                "Cache-block operation removed a static native translation.");
        state.lr = 0x4ff0;
        state.ctr = 0x4020;
        state.gpr[3] = 10;
        dispatcher.Execute(state, entry, 3, 2);
        Require(state.gpr[3] == (entry == 0x4000 ? 17u : 16u) &&
                    state.pc == 0x4ff0 && state.lr == 0x4ff0,
                "Compiled direct/indirect call, tail branch or return failed.");
    }
    Require(previousCalls == 0, "Native dispatcher used an unrelated callback.");
    ppc_call_guest(state, 0);
    Require(previousCalls == 1, "Native dispatcher did not restore the prior callback.");
    Reject([&] { dispatcher.Register(1, state, module); }, "already registered");
    Reject([&] { dispatcher.Register(2, state, {module[0], module[1]}); },
           "already registered");
    Reject([&] { dispatcher.Register(2, state, {{0x4080, nullptr, {0}}}); },
           "Invalid native");
    Reject([&] { dispatcher.Register(2, state, {{0x4081, ppc_00004040, {0}}}); },
           "Invalid native");
    Reject([&] { dispatcher.Register(2, state, {{0x4080, ppc_00004040, {1}}}); },
           "code mismatch");
    Reject([&] { dispatcher.Register(2, state, {
        {0x4080, ppc_00004040, {0}}, module[0]}); }, "already registered");
    Require(!dispatcher.Contains(0x4080), "Failed native registration partially committed.");
    for (const auto limits : {std::pair<size_t, size_t>{2, 2}, {3, 1}}) {
        state.lr = 0x4ff0;
        Reject([&] { dispatcher.Execute(state, 0x4000, limits.first, limits.second); },
               limits.first == 2 ? "budget exceeded" : "depth limit");
        ppc_call_guest(state, 0);
    }
    Require(previousCalls == 3, "Failed native execution did not restore the callback.");
    ppc_write_u32(state, 0x4020, 0);
    state.gpr[3] = 0x4020;
    ppc_0000B100(state);
    ppc_0000B120(state);
    state.gpr[3] = 0xc800u;
    ppc_00009350(state);
    ppc_00009360(state);
    ppc_00009370(state);
    state.lr = 0x4ff0;
    Reject([&] { dispatcher.Execute(state, 0x4000); }, "code mismatch at 0x00004020");
    ppc_write_u32(state, 0x4020, module[0].expectedWords[0]);
    dispatcher.Unregister(1);
    Require(!dispatcher.Contains(0x4020) && !dispatcher.Contains(0x4040) &&
                dispatcher.Contains(0x4000), "Native module removal left stale bindings.");
    state.lr = 0x4ff0;
    Reject([&] { dispatcher.Execute(state, 0x4000); },
           "No native translation registered at 0x00004020");
    Reject([&] { dispatcher.Unregister(1); }, "not registered");
    dispatcher.Register(1, state, module);
    state.lr = 0x4ff0;
    dispatcher.Execute(state, 0x4000);
    Require(state.pc == 0x4ff0, "Re-registering native module failed.");
    Reject([&] { dispatcher.Execute(state, 0x4001); }, "Invalid");
    state.lr = 0x4ff1;
    Reject([&] { dispatcher.Execute(state, 0x4000); }, "Invalid");
    state.lr = 0x4ff0;
    Reject([&] { dispatcher.Execute(state, 0x4000, 0); }, "Invalid");
    Reject([&] { dispatcher.Execute(state, 0x4000, 1, 0); }, "Invalid");
    dispatcher.Register(2, state, {{0x4080, NativeTestHook, {0}}});
    nativeTestAction = [&](PpcState&) { dispatcher.Unregister(1); };
    Reject([&] { dispatcher.Execute(state, 0x4080); }, "during execution");
    Require(dispatcher.Contains(0x4020), "Active native module was unregistered.");
    nativeTestAction = [&](PpcState&) { dispatcher.Register(3, state, root); };
    Reject([&] { dispatcher.Execute(state, 0x4080); }, "during execution");
    nativeTestAction = [&](PpcState&) { dispatcher.Execute(state, 0x4040); };
    Reject([&] { dispatcher.Execute(state, 0x4080); }, "reentrant");
    nativeTestAction = [](PpcState& guest) {
        PpcState unrelated;
        guest.callGuest(0x4040, unrelated);
    };
    Reject([&] { dispatcher.Execute(state, 0x4080); }, "call context");
    nativeTestAction = [](PpcState& guest) { guest.pc = 0x4080; };
    Reject([&] { dispatcher.Execute(state, 0x4080, 2); }, "budget exceeded");
    nativeTestAction = {};
    dispatcher.Unregister(2);
    state.callGuest = {};
    dispatcher.Execute(state, 0x4040);
    Require(!state.callGuest, "Native dispatcher did not restore an empty callback.");
}

} // namespace

int main() {
    try {
        TestLogicalRegisters();
        TestFloatingPointTransfers();
        TestFloatingPointMoves();
        TestFpscrWrites();
        TestFpscrBits();
        TestPerformanceMonitorResets();
        TestFpscrReads();
        TestSingleWidening();
        TestQuantizationRegisters();
        TestIndexedPairedAddresses();
        TestHardwareControlRegisters();
        TestCacheControlRegisters();
        TestPiInterruptMask();
        TestMiInterruptMask();
        TestDspControl();
        TestAiControl();
        TestExiStatus();
        TestExiTransferControl();
        TestExiInitializationRegisters();
        TestAndComplement();
        TestCacheBlockInstructions();
        TestL2ControlRegisters();
        TestRegisterAddCarry();
        TestPairedLoads(false);
        TestPairedLoads(true);
        TestIntegerDequantization();
        TestCompiledQuantizedLoads(false);
        TestCompiledQuantizedLoads(true);
        TestPairedMoves();
        TestCarryAndSpecialRegisters();
        TestLinkedBranchAndMultiLoad();
        TestLocalIndirectBranches();
        TestComparisonAndNor();
        TestCountLeadingZeros();
        TestSignExtendHalfword();
        TestSignExtendByteAndNegate();
        TestTimeBaseAndConditionXor();
        TestSubtractImmediateCarry();
        TestSubtractExtended();
        TestMultiplyDivideWords();
        TestNativeDispatch();
    } catch (const std::exception& error) {
        std::cerr << "Generated-code test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All compiled PowerPC translation tests passed.\n";
    return 0;
}
