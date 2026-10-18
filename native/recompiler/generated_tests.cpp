#include "generated_ppc_tests.h"
#include "native_dispatch.h"
#include "native_time_base.h"
#include "native_exi_bus.h"
#include "native_console.h"
#include "native_si_bus.h"
#include "native_sram.h"
#include "dsp_init_recompiler.h"
#include "ppc_recompiler.h"

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

void TestRelocatedOperands() {
    for (uint32_t base : {0x10000u, 0x18000u}) {
        for (uint32_t immediate : {0u, 0x7fffu, 0x8000u, 0xffffu}) {
            for (unsigned index = 0; index < 12; ++index) {
                PpcState state;
                state.memory.resize(0x20000);
                const auto pc = base + 0x900u + index * 0x10u;
                state.pc = pc;
                ppc_write_u16(state, pc + 2, static_cast<uint16_t>(immediate));
                const auto signedValue = static_cast<int32_t>(immediate ^ 0x8000u) - 0x8000;
                state.gpr[3] = 0x12345678u;
                state.gpr[4] = index < 8 ? 0x12345678u : 0x6000u - static_cast<uint32_t>(signedValue);
                state.fpr[3] = {ppc_widen_single_bits(0x40200000u), 0x12345678u};
                ppc_write_u32(state, 0x6000, 0x40600000u);
                RelocatedOperandFunctions[index](state);
                Require(state.pc == pc + 4, "Relocated native fallthrough used its compile-time PC.");
                if (index < 2) {
                    const uint32_t operand = index ? immediate << 16 : static_cast<uint32_t>(signedValue);
                    Require(state.gpr[3] == 0x12345678u + operand,
                            "Relocated addi/addis sign or high-half arithmetic differs.");
                } else if (index < 8) {
                    const auto operand = immediate << ((index & 1u) ? 16 : 0);
                    const auto expected = index < 4 ? 0x12345678u | operand :
                        index < 6 ? 0x12345678u ^ operand : 0x12345678u & operand;
                    Require(state.gpr[4] == expected, "Relocated logical operand or shifted form differs.");
                    if (index >= 6) {
                        Require((state.cr >> 28) == (expected == 0 ? 2u : (expected >> 31) ? 8u : 4u),
                                "Relocated andi/andis did not record CR0.");
                    }
                } else if (index == 8) {
                    Require(state.gpr[3] == 0x40600000u, "Relocated lwz address differs.");
                } else if (index == 9) {
                    Require(ppc_read_u32(state, 0x6000) == 0x12345678u, "Relocated stw address differs.");
                } else if (index == 10) {
                    const auto bits = ppc_widen_single_bits(0x40600000u);
                    Require(state.fpr[3] == PpcFpr{bits, bits}, "Relocated lfs address differs.");
                } else {
                    Require(ppc_read_u32(state, 0x6000) == 0x40200000u, "Relocated stfs address differs.");
                }
            }
        }
    }
}

void TestFloatingPointArithmetic() {
    for (unsigned index = 0; index < 4; ++index) {
        PpcState state;
        state.fpr[4] = {0x3ff0000000000000, 0x123456789abcdef0};
        state.fpscr = 0x60004;
        state.cr = 0x12345678;
        ReciprocalTestFunctions[index](state);
        Require(state.fpr[index % 2 ? 4 : 3] ==
                    PpcFpr{0x3fefff0000000000, 0x3fefff0000000000} &&
                    state.fpscr == 0x64004 &&
                    state.cr == (index >= 2 ? 0x10345678u : 0x12345678u),
                "Compiled fres destination alias, both result lanes, status or record bit differs.");
    }
    for (unsigned index = 0; index < 32; ++index) {
        PpcState state;
        state.fpr[4] = {0x3ff0000000000000, 0x4000000000000000};
        state.fpr[5] = {0x4000000000000000, 0x3ff0000000000000};
        state.cr = 0x12345678;
        const auto before = state.fpr;
        const auto field = index % 8;
        const uint32_t condition = index >= 16 ? 4 : 8;
        PairedCompareTestFunctions[index](state);
        Require(state.fpr == before &&
                    state.cr == ((0x12345678u & ~(15u << (28 - field * 4))) |
                                 (condition << (28 - field * 4))) &&
                    state.fpscr == condition << 12,
                "Compiled paired compare selected the wrong lane, CR field or FPSCR condition.");
        state.fpr[4] = {0x7ff8000000000001, 0x7ff8000000000001};
        state.fpscr = 0;
        PairedCompareTestFunctions[index](state);
        Require(((state.cr >> (28 - field * 4)) & 15) == 1 &&
                    (state.fpscr & 0x00080000u) == ((index / 8) % 2 ? 0x00080000u : 0),
                "Compiled paired comparison decoded ordered/unordered NaN behavior incorrectly.");
    }
    for (unsigned index = 0; index < 24; ++index) {
        PpcState state;
        state.fpr[4] = {0x4018000000000000, 0x4018000000000000};
        state.fpr[5] = {0x4000000000000000, 0x4000000000000000};
        state.fpr[6] = {0x4008000000000000, 0x4008000000000000};
        state.fpr[3].ps1 = 0x123456789abcdef0;
        state.fpscr = 4;
        state.cr = 0x12345678;
        const auto before = state.fpr;
        NegativeFusedTestFunctions[index](state);
        const unsigned destination = 3 + index % 4;
        const uint64_t expected = (index / 4) % 2 ? UINT64_C(0xc034000000000000) :
                                                                   UINT64_C(0xc030000000000000);
        Require(state.fpr[destination] == PpcFpr{expected, index < 16 ? expected : before[destination].ps1} &&
                    state.fpscr == 0x8004 &&
                    state.cr == (index % 2 ? 0x10345678u : 0x12345678u),
                "Compiled negative fused form/precision/source aliases or CR1 differs.");
    }
    for (unsigned index = 0; index < 8; ++index) {
        PpcState state;
        state.fpr[4] = {0x3ff0000000000000, 0x123456789abcdef0};
        state.fpr[3].ps1 = 0x123456789abcdef0;
        state.fpscr = 4;
        state.cr = 0x12345678;
        EstimateRoundTestFunctions[index](state);
        Require(state.fpr[index % 2 ? 4 : 3] ==
                    PpcFpr{index < 4 ? UINT64_C(0x3feffe8000000000) : UINT64_C(0x3ff0000000000000),
                           0x123456789abcdef0} &&
                    state.fpscr == 0x4004 &&
                    state.cr == (index % 4 >= 2 ? 0x10345678u : 0x12345678u),
                "Compiled frsqrte/frsp destination aliases, preserved ps1 or record bit differs.");
    }
    for (unsigned index = 0; index < 32; ++index) {
        PpcState state;
        state.fpr[4] = {ppc_widen_single_bits(0x40c00000), 0x0123456789abcdef};
        state.fpr[5] = {ppc_widen_single_bits(0x40000000), 0xfedcba9876543210};
        state.fpr[6] = {ppc_widen_single_bits(0x40400000), 0x123456789abcdef0};
        state.fpscr = 4;
        state.cr = 0x12345678;
        const auto destination = 3 + index % 4;
        const auto original = state.fpr;
        const auto expected = ppc_widen_single_bits((index / 8) % 2 ? 0x41a00000 : 0x41800000);
        ScalarFusedTestFunctions[index](state);
        Require(state.fpr[destination] == PpcFpr{expected, index < 16 ? expected : original[destination].ps1} &&
                    state.fpscr == 0x4004 &&
                    state.cr == ((index % 8) >= 4 ? 0x10345678u : 0x12345678u),
                "Compiled scalar fused precision/operand decoding, aliases or CR1 differs.");
    }
    for (unsigned index = 0; index < 32; ++index) {
        PpcState state;
        state.fpr[4] = {ppc_widen_single_bits(0x40c00000), ppc_widen_single_bits(0x41400000)};
        state.fpr[5] = {ppc_widen_single_bits(0x40000000), ppc_widen_single_bits(0x40800000)};
        state.fpr[6] = {ppc_widen_single_bits(0x40400000), ppc_widen_single_bits(0x40a00000)};
        state.fpscr = 4;
        state.cr = 0x12345678;
        const auto original = state.fpr;
        const uint32_t expected[][2] = {{0x41800000, 0x42600000}, {0x41a00000, 0x42800000},
                                        {0x41200000, 0x40a00000}, {0x40400000, 0x41200000}};
        PairedFusedSumTestFunctions[index](state);
        const auto destination = 3 + index % 4;
        Require(state.fpr[destination] == PpcFpr{ppc_widen_single_bits(expected[index / 8][0]),
                                                 ppc_widen_single_bits(expected[index / 8][1])} &&
                    state.fpscr == 0x4004 &&
                    state.cr == ((index % 8) >= 4 ? 0x10345678u : 0x12345678u),
                "Compiled fused/sum source decoding, alias preservation or CR1 record differs.");
        for (unsigned reg = 0; reg < 32; ++reg) {
            Require(reg == destination || state.fpr[reg] == original[reg],
                    "Compiled fused/sum changed a non-destination FPR.");
        }
    }
    for (unsigned index = 0; index < 36; ++index) {
        PpcState paired;
        paired.fpr[4] = {0x4018000000000000, 0x4028000000000000}; // 6, 12
        paired.fpr[5] = {0x4000000000000000, 0x4010000000000000}; // 2, 4
        paired.fpscr = 4;
        paired.cr = 0x12345678;
        const PpcFpr expected[] = {
            {0x4008000000000000, 0x4008000000000000},
            {0x4010000000000000, 0x4020000000000000},
            {0x4020000000000000, 0x4030000000000000},
            {0x4028000000000000, 0x4048000000000000},
            {0x4028000000000000, 0x4038000000000000},
            {0x4038000000000000, 0x4048000000000000}
        };
        const auto before = paired.fpr;
        PairedArithmeticTestFunctions[index](paired);
        const unsigned destination = 3 + index % 3;
        Require(paired.fpr[destination] == expected[index / 6] && paired.fpscr == 0x4004 &&
                    paired.cr == ((index % 6) >= 3 ? 0x10345678u : 0x12345678u),
                "Compiled paired arithmetic source/lane decoding, aliases, NI or CR1 differs.");
        for (unsigned reg = 0; reg < 32; ++reg) {
            Require(reg == destination || paired.fpr[reg] == before[reg],
                    "Compiled paired arithmetic changed a non-destination FPR.");
        }
    }
    for (unsigned index = 0; index < 32; ++index) {
        PpcState state;
        state.fpr[4] = {0x4008000000000000, 0x0123456789abcdef};
        state.fpr[5] = {0x4000000000000000, 0xfedcba9876543210};
        state.fpr[3].ps1 = 0x123456789abcdef0;
        state.fpscr = 4;
        state.cr = 0x12345678;
        FloatArithmeticTestFunctions[index](state);
        const unsigned dest = index % 2 ? 4 : 3;
        const uint64_t values[] = {0x3ff8000000000000, 0x3ff0000000000000,
                                   0x4014000000000000, 0x4018000000000000};
        const auto expected = values[(index % 16) / 4];
        const uint64_t paired = index < 16 ? expected :
            dest == 4 ? UINT64_C(0x0123456789abcdef) : UINT64_C(0x123456789abcdef0);
        Require(state.fpr[dest] == PpcFpr{expected, paired} && state.fpscr == 0x4004 &&
                    state.cr == ((index % 4) >= 2 ? 0x10345678u : 0x12345678u),
                "Generated arithmetic source decoding, aliasing, precision, NI or CR1 record differs.");
    }
    for (unsigned field = 0; field < 8; ++field) {
        PpcState state;
        state.fpr[4].ps0 = 0xbff0000000000000;
        state.fpr[5].ps0 = 0x3ff0000000000000;
        state.cr = 0x12345678;
        state.fpscr = 0x60004;
        FloatCompareTestFunctions[field](state);
        const unsigned shift = 28 - field * 4;
        Require(state.cr == ((0x12345678u & ~(15u << shift)) | (8u << shift)) &&
                    state.fpscr == 0x68004,
                "Generated comparison did not preserve other CR fields and FPSCR status.");
    }
    PpcState state;
    state.fpr[4] = {0x400e000000000000, 0};
    state.fpr[3].ps1 = 0x123456789abcdef0;
    state.cr = 0x12345678;
    ppc_0000F280(state);
    Require(state.fpr[3] == PpcFpr{0xfff8000000000003, 0x123456789abcdef0} &&
                state.fpscr == 0x82020000 && state.cr == 0x18345678,
            "Generated truncating integer conversion or CR1 record differs.");
    state.fpscr = 0;
    state.fpr[4].ps0 = 0x7ff8000000000000;
    ppc_0000F290(state);
    Require(state.fpscr == 0xa0081000 && ((state.cr >> 16) & 15) == 1,
            "Generated ordered NaN comparison did not raise VXVC and unordered CR3.");
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

void TestYieldingBranchesAndPrecisePc() {
    for (uint32_t count : {1u, 2u, UINT32_MAX}) {
        PpcState state;
        state.gpr[3] = 0;
        state.gpr[4] = 0x12345678u;
        state.ctr = count;
        state.cr = 0x13572468u;
        state.xer = 0xe0000000u;
        ppc_0000B400(state);
        Require(state.gpr[3] == 1 && state.gpr[4] == 0x12345678u &&
                    state.ctr == count - 1 && state.cr == 0x13572468u &&
                    state.xer == 0xe0000000u &&
                    state.pc == (count == 1 ? 0xb408u : 0xb400u),
                "Yielding bdnz did not update CTR exactly once or executed past its boundary.");
    }
    PpcState state;
    state.gpr[4] = 0x12345678u;
    ppc_0000B420(state);
    Require(state.pc == 0xb420u && state.gpr[4] == 0x12345678u,
            "A direct self branch did not yield.");
    for (uint32_t target : {0xb430u, 0xb434u, 0x1000u}) {
        state.ctr = target | 3u;
        ppc_0000B430(state);
        Require(state.pc == target && state.ctr == (target | 3u) &&
                    state.gpr[4] == 0x12345678u,
                "A local or external CTR branch did not yield with target alignment preserved.");
    }
    for (bool taken : {false, true}) {
        state.cr = taken ? 0xffffffffu : 0;
        state.lr = 0xb443u;
        ppc_0000B440(state);
        Require(state.pc == (taken ? 0xb440u : 0xb444u) && state.lr == 0xb443u &&
                    state.gpr[4] == 0x12345678u,
                "Conditional LR branch did not yield on both paths.");
        ppc_0000B450(state);
        Require(state.pc == (taken ? 0xb450u : 0xb454u) &&
                    state.gpr[4] == 0x12345678u,
                "Conditional direct self branch did not yield on both paths.");
    }
    for (const auto& entry : {
             std::make_pair(0xb460u, ppc_0000B460),
             std::make_pair(0xb480u, ppc_0000B480),
             std::make_pair(0xb490u, ppc_0000B490)}) {
        unsigned calls = 0;
        state.ctr = entry.first + 8u;
        state.lr = entry.first + 8u;
        state.callGuest = [&](uint32_t target, PpcState& guest) {
            Require(++calls == 1 && target == entry.first + 8u &&
                        guest.lr == entry.first + 4u,
                    "Yielding linked branch changed its original target or continuation.");
            guest.pc = guest.lr;
        };
        entry.second(state);
        Require(calls == 1 && state.pc == entry.first + 4u &&
                    state.gpr[4] == 0x12345678u,
                "Linked branch did not return to dispatch after its guest call.");
    }
    for (uint32_t count : {1u, 2u}) {
        unsigned calls = 0;
        state.ctr = count;
        state.callGuest = [&](uint32_t target, PpcState& guest) {
            Require(++calls == 1 && target == 0xb4c8u && guest.lr == 0xb4c4u,
                    "Conditional linked branch called the wrong target.");
            guest.pc = guest.lr;
        };
        ppc_0000B4C0(state);
        Require(calls == (count == 1 ? 0u : 1u) && state.ctr == count - 1 &&
                    state.pc == 0xb4c4u && state.lr == 0xb4c4u &&
                    state.gpr[4] == 0x12345678u,
                "Conditional linked branch did not yield after exactly one CTR test.");
    }
    for (bool taken : {false, true}) {
        state.cr = taken ? 0xffffffffu : 0;
        state.ctr = 0xb4e3u;
        ppc_0000B4E0(state);
        Require(state.pc == (taken ? 0xb4e0u : 0xb4e4u) &&
                    state.ctr == 0xb4e3u && state.gpr[4] == 0x12345678u,
                "Conditional CTR branch did not yield on both paths without decrementing CTR.");
    }
    state.callGuest = {};
    state.memory.resize(0xc000u);
    ppc_write_u32(state, 0xb420u, 0x48000000u);
    ppc_write_u32(state, 0xb424u, 0x38800001u);
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, {{0xb420u, ppc_0000B420, {0x48000000u, 0x38800001u}}});
    state.lr = 0xb500u;
    bool rejected = false;
    try {
        dispatcher.Execute(state, 0xb420u, 7);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("budget exceeded") != std::string::npos;
    }
    Require(rejected && state.pc == 0xb420u && !state.callGuest,
            "A real compiled self loop escaped the dispatch budget or retained its callback.");
    state.gpr[3] = 0;
    state.gpr[4] = UINT32_MAX;
    rejected = false;
    try {
        ppc_0000B4A0(state);
    } catch (const std::out_of_range&) {
        rejected = true;
    }
    Require(rejected && state.pc == 0xb4a4u && state.gpr[3] == 1,
            "Precise PC did not identify a failed straight-line memory instruction.");
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

void TestSingleTransfers() {
    using Transfer = void (*)(PpcState&);
    const Transfer transfers[] = {ppc_0000B700, ppc_0000B710, ppc_0000B720, ppc_0000B730,
                                  ppc_0000B760, ppc_0000B770, ppc_0000B780, ppc_0000B790};
    for (uint32_t bits : {0u, 0x80000000u, 1u, 0x807fffffu, 0x00800000u, 0x3f800000u,
                          0xbf800000u, 0x7f7fffffu, 0x7f800000u, 0xff800000u,
                          0x7fc12345u, 0xff812345u}) {
        for (unsigned operation = 0; operation < 8; ++operation) {
            PpcState state;
            state.memoryMap = PpcMemoryMap::GameCubeRam;
            state.memory.resize(kGameCubeRamSize, 0xcc);
            state.gpr[3] = 0xc0000104u;
            state.gpr[4] = 0xfffffffcu;
            state.fpr[3] = {ppc_widen_single_bits(bits), UINT64_C(0xdeadbeef12345678)};
            state.fpscr = 0x12345678u;
            state.cr = 0x87654321u;
            ppc_write_u32(state, 0x80000100u, operation % 4 < 2 ? bits : 0xa5a5a5a5u);
            const auto before = state;
            transfers[operation](state);
            Require(state.gpr[3] == (operation % 2 ? 0xc0000100u : before.gpr[3]) &&
                        state.fpscr == before.fpscr && state.cr == before.cr &&
                        state.fpr[3].ps0 == ppc_widen_single_bits(bits) &&
                        state.fpr[3].ps1 == (operation % 4 < 2 ? ppc_widen_single_bits(bits) :
                                                          before.fpr[3].ps1) &&
                        ppc_read_u32(state, 0x100u) == bits,
                    "Compiled single transfer lost raw bits, paired lane, alias, update or FPSCR state.");
            auto expected = before.memory;
            const auto offset = ppc_memory_offset(state, 0x100u, 4);
            for (unsigned byte = 0; byte < 4; ++byte) {
                expected[offset + byte] = static_cast<uint8_t>(bits >> (24 - byte * 8));
            }
            Require(state.memory == expected, "Compiled single transfer wrote outside its four-byte destination.");
        }
    }
    for (unsigned operation = 0; operation < 8; ++operation) {
        for (uint32_t base : {0x80000105u, 0x81800004u, 0xfffffffcu}) {
            PpcState state;
            state.memoryMap = PpcMemoryMap::GameCubeRam;
            state.memory.resize(kGameCubeRamSize, 0xcc);
            state.gpr[3] = base;
            state.gpr[4] = 0xfffffffcu;
            state.fpr[3] = {UINT64_C(0x3ff0000000000000), UINT64_C(0x4000000000000000)};
            const auto before = state;
            bool failed = false;
            try {
                transfers[operation](state);
            } catch (const std::runtime_error&) {
                failed = true;
            } catch (const std::out_of_range&) {
                failed = true;
            }
            Require(failed && state.gpr[3] == before.gpr[3] && state.fpr == before.fpr &&
                        state.memory == before.memory && state.fpscr == before.fpscr,
                    "Invalid single transfer committed an update, FPR or partial memory write.");
        }
    }
    PpcState absolute;
    absolute.memory.resize(0x108u, 0xa5);
    absolute.gpr[0] = 0xffffffffu;
    ppc_write_u32(absolute, 0x100u, 0x80000001u);
    ppc_0000B740(absolute);
    Require(absolute.fpr[3].ps0 == UINT64_C(0xb6a0000000000000) &&
                absolute.fpr[3].ps1 == absolute.fpr[3].ps0,
            "Absolute single load used GPR0 or incorrectly widened a signed subnormal.");
    absolute.fpr[3].ps0 = UINT64_C(0x3ff000001fffffff);
    ppc_0000B750(absolute);
    Require(ppc_read_u32(absolute, 0x100u) == 0x3f800000u &&
                ppc_read_u32(absolute, 0x104u) == 0xa5a5a5a5u,
            "Single store did not truncate the mantissa or wrote eight bytes.");
    absolute.gpr[4] = 0x100;
    ppc_0000B7A0(absolute);
    Require(absolute.fpr[3].ps0 == UINT64_C(0x3ff0000000000000),
            "Indexed single load used nonzero GPR0.");
    absolute.fpr[3].ps0 = UINT64_C(0xbff0000000000000);
    ppc_0000B7B0(absolute);
    Require(ppc_read_u32(absolute, 0x100) == 0xbf800000u, "Indexed single store used nonzero GPR0.");
    absolute.gpr[3] = 0x80;
    ppc_0000B7C0(absolute);
    Require(absolute.gpr[3] == 0x100 && absolute.fpr[3].ps0 == UINT64_C(0xbff0000000000000),
            "Indexed single update load lost an aliased address register.");
    absolute.gpr[3] = 0x80;
    ppc_0000B7D0(absolute);
    Require(absolute.gpr[3] == 0x100 && ppc_read_u32(absolute, 0x100) == 0xbf800000u,
            "Indexed single update store lost an aliased address register.");
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

void TestPairedStores() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(512, 0xa5);
    const uint32_t bits[] = {0, 0x80000000u, 1, 0x007fffffu, 0x00800000u,
                            0x3f800000u, 0x7f7fffffu, 0x7f800000u, 0x7f812345u};
    for (unsigned index = 0; index < 64; ++index) {
        const auto gqr = (index % 32) / 4;
        const bool single = (index & 2u) != 0, update = (index & 1u) != 0;
        for (const auto alias : {0u, 0x80000000u, 0xc0000000u}) {
            for (const auto first : bits) {
                state.gqr.fill(7);
                state.gqr[gqr] = 0xffff3f00u;
                state.gpr[4] = alias + 0x104u;
                state.gpr[5] = 0xfffffffcu;
                state.fpr[3] = {ppc_widen_single_bits(first), ppc_widen_single_bits(0x40000000)};
                state.fpscr = 0x12345678;
                state.cr = 0x87654321;
                state.xer = 0xe0000000;
                ppc_write_u32(state, 0x104, 0xa5a5a5a5);
                const auto fpr = state.fpr;
                PairedStoreTestFunctions[index](state);
                Require(ppc_read_u32(state, 0x100) == first &&
                            ppc_read_u32(state, 0x104) == (single ? 0xa5a5a5a5u : 0x40000000u) &&
                            state.gpr[4] == alias + (update ? 0x100u : 0x104u) &&
                            state.fpr == fpr && state.fpscr == 0x12345678 &&
                            state.cr == 0x87654321 && state.xer == 0xe0000000 &&
                            state.memory[0xff] == 0xa5 && state.memory[0x108] == 0xa5 &&
                            state.pc == 0xe004u + index * 16,
                        "Compiled paired store lost lane bits, W/I/address/update fields or guards.");
            }
        }
        state.gpr[4] = 0x203;
        const auto memory = state.memory;
        bool rejected = false;
        try {
            PairedStoreTestFunctions[index](state);
        } catch (const std::out_of_range&) {
            rejected = true;
        }
        Require(rejected && state.gpr[4] == 0x203 && state.memory == memory,
                "Paired store boundary fault partially wrote RAM or updated base.");
        state.gpr[4] = 0x104;
        state.gqr[gqr] = 4;
        rejected = false;
        try {
            PairedStoreTestFunctions[index](state);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("integer quantization") != std::string::npos;
        }
        Require(rejected && state.gpr[4] == 0x104 && state.memory == memory,
                "Unsupported integer-quantized store changed RAM or base.");
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
            for (uint32_t mode : {0u, 2u, 0x1000u, 0x2000u, 0x2002u, 0x8000u,
                                  0x8002u, 0x9002u, 0xa000u, 0xa002u, 0xb002u}) {
                const uint32_t desired = mode | (initial & ~0xb002u);
                state.gpr[3] = desired;
                ppc_00009310(state);
                ppc_00009300(state);
                Require(state.gpr[3] == desired && state.msr == (desired & ~0x8000u) &&
                            state.externalInterruptsEnabled == ((desired & 0x8000u) != 0),
                        "Compiled MSR transfer did not preserve FP/ME/EE/RI state.");
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
            const uint32_t allowed = msr ? 0xb002u : 0xa0000000u;
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
    for (auto function : {ppc_0000B100, ppc_0000B110, ppc_0000B120, ppc_0000B130, ppc_0000B140, ppc_0000B150}) {
        const bool indexed = function == ppc_0000B110 || function == ppc_0000B130 || function == ppc_0000B150;
        const uint32_t pc = function == ppc_0000B100 ? 0xb100u :
            function == ppc_0000B110 ? 0xb110u : function == ppc_0000B120 ? 0xb120u :
            function == ppc_0000B130 ? 0xb130u : function == ppc_0000B140 ? 0xb140u : 0xb150u;
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
                const uint32_t expected = (initial & (channel == 2 ? 0xau :
                    channel == 0 ? 0x380au : 0x180au)) | value;
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
    Require(state.exiStatus[0] == 0x387au,
            "Original interrupt masking did not retain sticky ROMDIS and pending/presence state.");
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

void TestExiImmediateTransactions() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    const auto ram = state.memory;
    state.exiImmediateEnabled = true;
    state.exiStatus = {0x2000u, 0u, 0u};
    state.exiTransferControl = {0u, 0u, 0u};
    for (unsigned channel = 0; channel < 3; ++channel) {
        const uint32_t base = 0xcc006800u + channel * 0x14u;
        for (unsigned device = 0; device < 3; ++device) {
            const uint32_t select = (0x80u << device) | 0x50u;
            ppc_write_u32(state, base, select);
            Require(ppc_read_u32(state, base) == (select | (channel == 0 ? 0x2000u : 0)),
                    "EXI selected device, clock or sticky ROMDIS differs.");
            for (unsigned length = 1; length <= 4; ++length) {
                for (unsigned direction = 0; direction < 3; ++direction) {
                    unsigned calls = 0;
                    state.transferExiImmediate = [&](const ExiImmediateRequest& request) {
                        Require(++calls == 1 && request.channel == channel &&
                                    request.device == device && request.length == length &&
                                    request.direction == direction && request.data == 0x12345678u,
                                "EXI immediate request differs from its selected backend.");
                        return 0xa1b2c3d4u;
                    };
                    ppc_write_u32(state, base + 0x10u, 0x12345678u);
                    const uint32_t control = ((length - 1) << 4) | (direction << 2) | 1u;
                    ppc_write_u32(state, base + 0xcu, control);
                    Require(calls == 1 && ppc_read_u32(state, base + 0x10u) ==
                                (direction == 1 ? 0x12345678u : 0xa1b2c3d4u) &&
                                ppc_read_u32(state, base + 0xcu) == (control & ~1u) &&
                                (ppc_read_u32(state, base) & 8u) != 0 && state.memory == ram,
                            "EXI transfer did not commit data, completion and TCINT.");
                    ppc_write_u32(state, base, select | 8u);
                    Require((ppc_read_u32(state, base) & 8u) == 0,
                            "EXI transfer completion was not acknowledged with W1C.");
                }
            }
            const auto status = state.exiStatus;
            const auto controls = state.exiTransferControl;
            const auto data = state.exiImmediateData;
            const auto reject = [&](uint32_t control) {
                bool rejected = false;
                try {
                    ppc_write_u32(state, base + 0xcu, control);
                } catch (const std::runtime_error&) {
                    rejected = true;
                }
                Require(rejected && state.exiStatus == status &&
                            state.exiTransferControl == controls &&
                            state.exiImmediateData == data && state.memory == ram,
                        "Rejected EXI transaction mutated state or fabricated completion.");
            };
            state.transferExiImmediate = {};
            reject(0x15u);
            state.transferExiImmediate = [](const ExiImmediateRequest&) -> uint32_t {
                throw std::runtime_error("test peripheral failure");
            };
            reject(0x15u);
            unsigned calls = 0;
            state.transferExiImmediate = [&](const ExiImmediateRequest&) {
                ++calls;
                return 0;
            };
            for (uint32_t invalid : {0u, 2u, 3u, 0xdu, 0x41u, UINT32_MAX}) reject(invalid);
            Require(calls == 0, "Invalid EXI transaction reached its backend.");
        }
        ppc_write_u32(state, base, 0);
        Require(state.exiStatus[channel] == (channel == 0 ? 0x2000u : 0u),
                "EXI deselection did not clear chip selects and preserve sticky ROMDIS.");
    }
}

void TestSiStartupHardwareAndArithmetic() {
    for (uint32_t value : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu, 0x80000020u}) {
        for (uint32_t shift = 0; shift < 128; ++shift) {
            for (bool record : {false, true}) {
                PpcState state;
                state.gpr[3] = value;
                state.gpr[5] = shift;
                state.xer = 0xe0000001u;
                state.cr = 0x12345678u;
                const unsigned count = shift & 63u;
                const int64_t signedValue = value < 0x80000000u ?
                    static_cast<int64_t>(value) : static_cast<int64_t>(value) - 0x100000000ll;
                const int64_t divisor = int64_t{1} << std::min(count, 31u);
                const uint32_t expected = static_cast<uint32_t>(count >= 32 ?
                    (signedValue < 0 ? -1 : 0) :
                    signedValue / divisor - (signedValue < 0 && signedValue % divisor != 0 ? 1 : 0));
                const bool carry = signedValue < 0 &&
                    (count >= 32 || (count != 0 && uint64_t{value} % (uint64_t{1} << count) != 0));
                if (record) ppc_0000B530(state); else ppc_0000B520(state);
                const uint32_t cr0 = ((expected == 0 ? 2u : expected < 0x80000000u ? 4u : 8u) | 1u) << 28;
                Require(state.gpr[4] == expected && state.gpr[3] == value &&
                            state.gpr[5] == shift && state.xer == (0xc0000001u | (carry ? 0x20000000u : 0u)) &&
                            state.cr == (record ? cr0 | 0x02345678u : 0x12345678u),
                        "Compiled sraw result, six-bit count, XER carry or CR0 mismatch.");
            }
        }
        for (bool record : {false, true}) {
            PpcState state;
            state.gpr[3] = value;
            state.gpr[5] = 0xffffffffu;
            state.xer = 0xc0000001u;
            state.cr = 0x12345678u;
            auto reference = state;
            const auto expected = ppc_shift_right_arithmetic(reference, value, 5);
            if (record) ppc_0000B550(state); else ppc_0000B540(state);
            const auto cr0 = ((expected == 0 ? 2u : expected < 0x80000000u ? 4u : 8u) | 1u) << 28;
            Require(state.gpr[4] == expected && state.xer == reference.xer &&
                        state.cr == (record ? cr0 | 0x02345678u : 0x12345678u),
                    "Compiled srawi did not use its immediate shift or record flags.");
        }
        for (uint32_t right : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
            for (bool record : {false, true}) {
                for (bool subtract : {false, true}) {
                    PpcState state;
                    state.gpr[3] = value;
                    state.gpr[4] = value;
                    state.gpr[5] = right;
                    state.xer = 0xe0000001u;
                    state.cr = 0x12345678u;
                    const auto expected = subtract ? right - value : value ^ right;
                    if (subtract) {
                        if (record) ppc_0000B590(state); else ppc_0000B580(state);
                    } else {
                        if (record) ppc_0000B570(state); else ppc_0000B560(state);
                    }
                    const auto cr0 = ((expected == 0 ? 2u : expected < 0x80000000u ? 4u : 8u) | 1u) << 28;
                    Require(state.gpr[subtract ? 3 : 4] == expected &&
                                state.xer == (subtract ? 0xc0000001u | (right >= value ? 0x20000000u : 0u) :
                                                         0xe0000001u) &&
                                state.cr == (record ? cr0 | 0x02345678u : 0x12345678u),
                            "Compiled xor/subfc result, XER or CR0 mismatch.");
                }
            }
        }
    }
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    Reject([&] { ppc_read_u32(state, 0xcc006434u); }, "address");
    Reject([&] { ppc_read_u16(state, 0xcc00206cu); }, "address");
    state.viDtvStatus = 0;
    Require(ppc_read_u16(state, 0xcc00206cu) == 0, "Explicit analog VI profile differs.");
    state.viDtvStatus = 1;
    Require(ppc_read_u16(state, 0xcc00206cu) == 1, "VI digital-cable flag differs.");
    Reject([&] { ppc_write_u16(state, 0xcc00206cu, 0); }, "address");
    state.serialInterface.emplace();
    ppc_write_u32(state, 0xcc006430u, 0x00f60200u);
    Require(ppc_read_u32(state, 0xcc006430u) == 0x00f60200u, "SI polling timing was not retained.");
    Reject([&] { ppc_write_u32(state, 0xcc006430u, 0x80); }, "automatic polling");
    Reject([&] { ppc_write_u32(state, 0xcc006430u, 0x80000000u); }, "polling");
    state.serialInterface->communication = 0xb0000000u;
    ppc_write_u32(state, 0xcc006434u, 0xc0010300u);
    Require(ppc_read_u32(state, 0xcc006434u) == 0x70010300u,
            "SI transfer-complete W1C or interrupt-mask semantics differ.");
    Reject([&] { ppc_write_u32(state, 0xcc006434u, 0xc0010301u); }, "backend");
    Require(state.serialInterface->communication == 0x70010300u,
            "Missing SI backend fabricated a transfer or completion.");
    Reject([&] { ppc_write_u32(state, 0xcc006434u, 0x80u); }, "control");
    state.serialInterface->status = 0x0f0f0f0fu;
    ppc_write_u32(state, 0xcc006438u, 0x01020408u);
    Require(ppc_read_u32(state, 0xcc006438u) == 0x0e0d0b07u, "SI per-channel error W1C differs.");
    Reject([&] { ppc_write_u32(state, 0xcc006438u, 0x80000000u); }, "polling");
    for (unsigned index = 0; index < 32; ++index) {
        const auto address = 0xcc006480u + index * 4u;
        ppc_write_u32(state, address, index * 0x01020304u);
        Require(ppc_read_u32(state, address) == index * 0x01020304u, "SI communication RAM differs.");
    }
    Reject([&] { ppc_read_u32(state, 0xcc006481u); }, "address");
    Reject([&] { ppc_write_u32(state, 0xcc006500u, 0); }, "address");
    state.serialInterface->communication = 1;
    Reject([&] { ppc_read_u32(state, 0xcc006434u); }, "completion backend");
    Reject([&] { ppc_write_u32(state, 0xcc006480u, 0); }, "pending");
}

void TestCompiledDspTask() {
    const auto stateFor = [](const std::vector<uint16_t>& words) {
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.dspControl = 0x100;
        state.dspHardware.emplace();
        auto& dsp = *state.dspHardware;
        dsp.task.emplace();
        dsp.task->mode = NativeDspTaskMode::Running;
        dsp.programHalted = false;
        dsp.dataRam.fill(0xa55a);
        std::copy(words.begin(), words.end(), dsp.task->instructionRam.begin());
        return state;
    };
    auto state = stateFor(SyntheticTaskWords);
    auto& dsp = *state.dspHardware;
    auto& task = *dsp.task;
    state.executeDspTask = SyntheticDspTask;
    NativeDspAdvanceTask(state);
    Require(dsp.fromDspMailbox == 0xa3456789 && dsp.dataRam[0x20] == 0x1234 &&
                dsp.dataRam[0x1f] == 0xa55a && dsp.dataRam[0x21] == 0xa55a &&
                dsp.instructionPc == 22 && !dsp.programHalted && dsp.completedPrograms == 0 &&
                task.registers[18] == 0xff && (task.registers[19] & 0xff00u) == 0x3a00 &&
                task.interrupts == 1 && dsp.sentMail == 1 && *state.dspControl == 0x180,
            "Compiled task did not derive memory stores, mail and IRQ from its own instructions.");
    const auto executed = dsp.executedInstructions;
    NativeDspAdvanceTask(state);
    Require(dsp.instructionPc == 22 && dsp.executedInstructions == executed + 3 && dsp.sentMail == 1,
            "Compiled task mailbox wait did not yield or overwrote pending mail.");
    ppc_write_u16(state, 0xcc00500au, 0x104);
    NativeDspAdvanceTask(state);
    Require(dsp.programHalted && dsp.instructionPc == 22 && dsp.executedInstructions == executed + 3,
            "CPU HALT did not pause the compiled DSP task.");
    ppc_write_u16(state, 0xcc00500au, 0x100);
    Require(!dsp.programHalted && task.mode == NativeDspTaskMode::Running && dsp.instructionPc == 22 &&
                dsp.executedInstructions == executed + 3 && dsp.sentMail == 1,
            "CPU resume restarted the task loader or lost compiled task state.");
    Require(ppc_read_u16(state, 0xcc005004u) == 0xa345 &&
                ppc_read_u16(state, 0xcc005006u) == 0x6789 && dsp.consumedMail == 1,
            "CPU did not consume actual compiled task output.");
    NativeDspAdvanceTask(state);
    Require(dsp.instructionPc == 27 && !dsp.programHalted &&
                task.cpuMailConsumed == 0, "Task input wait invented a CPU command.");
    ppc_write_u16(state, 0xcc005000u, 0x9234);
    Require((dsp.toDspMailbox & 0x80000000u) == 0 && task.cpuMailSent == 0,
            "CPU mailbox high word committed an incomplete message.");
    ppc_write_u16(state, 0xcc005002u, 0x5678);
    Reject([&] { ppc_write_u16(state, 0xcc005000u, 0); }, "active");
    Require(dsp.toDspMailbox == 0x92345678 && task.cpuMailSent == 1, "CPU mailbox message was overwritten.");
    NativeDspAdvanceTask(state);
    Require(dsp.programHalted && dsp.completedPrograms == 1 &&
                task.mode == NativeDspTaskMode::Halted && dsp.dataRam[0x21] == 0x5678 &&
                task.cpuMailConsumed == 1 && dsp.toDspMailbox == 0x12345678 &&
                dsp.consumedMail == 1 && *state.dspControl == 0x184,
            "Compiled task did not consume CPU input, store it and execute its real HALT.");
    Reject([&] { ppc_write_u16(state, 0xcc005002u, 0); }, "active");
    for (unsigned offset : {0u, 9u}) {
        auto damaged = stateFor(SyntheticTaskWords);
        damaged.executeDspTask = SyntheticDspTask;
        damaged.dspHardware->instructionPc = offset == 9 ? 8 : 0;
        damaged.dspHardware->task->instructionRam[offset] ^= 1;
        const auto before = *damaged.dspHardware;
        Reject([&] { NativeDspAdvanceTask(damaged); }, "code mismatch");
        Require(damaged.dspHardware->dataRam == before.dataRam &&
                    damaged.dspHardware->task->registers == before.task->registers &&
                    damaged.dspHardware->executedInstructions == before.executedInstructions,
                "Damaged DSP code committed instruction effects before integrity failure.");
    }
    auto signedState = stateFor({0x8f00, 0x009e, 0x8001, 0x0021});
    signedState.executeDspTask = SignedDspTask;
    NativeDspAdvanceTask(signedState);
    Require(signedState.dspHardware->task->registers[16] == 0xffff &&
                signedState.dspHardware->task->registers[28] == 0 &&
                signedState.dspHardware->task->registers[30] == 0x8001,
            "DSP S40 mid-register load did not sign-extend and clear the accumulator low word.");
    Require(NativeDspTaskReadRegister(signedState, 30) == 0x8001, "DSP in-range S40 mid read was saturated.");
    NativeDspTaskWriteRegister(signedState, 16, 0);
    Require(NativeDspTaskReadRegister(signedState, 30) == 0x7fff, "DSP positive S40 overflow did not saturate.");
    NativeDspTaskWriteRegister(signedState, 16, 0xff);
    NativeDspTaskWriteRegister(signedState, 19, 0);
    NativeDspTaskWriteRegister(signedState, 30, 0x7ffe);
    NativeDspTaskWriteRegister(signedState, 19, 0x4000);
    Require(NativeDspTaskReadRegister(signedState, 30) == 0x8000, "DSP negative S40 overflow did not saturate.");
    Reject([&] { NativeDspTaskReadRegister(signedState, 12); }, "stack");
    Reject([&] { NativeDspTaskWriteRegister(signedState, 12, 1); }, "stack");
    auto commandState = stateFor(SyntheticCommandWords);
    commandState.memory.assign(128, 0xa5);
    commandState.executeDspTask = SyntheticDspCommand;
    ppc_write_u32(commandState, 0x20, 0x00021234);
    NativeDspAdvanceTask(commandState);
    Require(commandState.dspHardware->instructionPc == 14 &&
                commandState.dspHardware->task->cpuMailConsumed == 0,
            "Compiled command header wait fabricated a command.");
    const auto send = [&](uint32_t mail) {
        ppc_write_u16(commandState, 0xcc005000, static_cast<uint16_t>(mail >> 16));
        ppc_write_u16(commandState, 0xcc005002, static_cast<uint16_t>(mail));
    };
    send(0x92340004);
    NativeDspAdvanceTask(commandState);
    Require(commandState.dspHardware->instructionPc == 24 &&
                commandState.dspHardware->task->cpuMailConsumed == 1 &&
                commandState.dspHardware->task->dataTransfers == 0,
            "Compiled command did not wait for its separately supplied pointer.");
    send(0x80000020);
    for (unsigned i = 0; i < 8 && !commandState.dspHardware->programHalted; ++i) {
        NativeDspAdvanceTask(commandState);
    }
    const auto& commandDsp = *commandState.dspHardware;
    Require(commandDsp.programHalted && commandDsp.fromDspMailbox == 0xc56789ab &&
                commandDsp.task->cpuMailConsumed == 2 && commandDsp.task->dataTransfers == 2 &&
                commandDsp.task->transferredBytes == 10 && commandDsp.task->returnAddresses.empty() &&
                commandDsp.task->loops.empty() && commandDsp.completedPrograms == 1 &&
                commandDsp.dataRam[0x100] == 2 && commandDsp.dataRam[0x101] == 0x1234 &&
                commandDsp.dataRam[0xff] == 0xa55a && commandDsp.dataRam[0x102] == 0xa55a &&
                commandDsp.dataRam[0x110] == 7 && commandDsp.dataRam[0x112] == 7 &&
                commandDsp.dataRam[0x113] == 0xa55a && commandDsp.dataRam[0x120] == 0 &&
                commandDsp.dataRam[0x121] == 0xfff8 &&
                ppc_read_u32(commandState, 0x40) == 0x00070007 &&
                ppc_read_u16(commandState, 0x44) == 7 &&
                commandState.memory[0x3f] == 0xa5 && commandState.memory[0x46] == 0xa5,
            "Compiled mail/DMA/call/parallel/loop/table-dispatch chain produced wrong original-data effects.");
    auto badHeader = stateFor(SyntheticCommandWords);
    badHeader.executeDspTask = SyntheticDspCommand;
    badHeader.dspHardware->toDspMailbox = 0xabcd0004;
    NativeDspAdvanceTask(badHeader);
    Require(badHeader.dspHardware->instructionPc == 14 &&
                badHeader.dspHardware->task->cpuMailConsumed == 0 &&
                badHeader.dspHardware->task->dataTransfers == 0,
            "Compiled comparison accepted a wrong command header.");
    for (uint16_t count : {uint16_t(0), uint16_t(1), uint16_t(600)}) {
        auto loop = stateFor(SyntheticLoopWords);
        loop.executeDspTask = SyntheticDspLoop;
        loop.dspHardware->task->registers[2] = count;
        NativeDspAdvanceTask(loop);
        if (count == 600) {
            Require(!loop.dspHardware->programHalted && loop.dspHardware->task->loops.size() == 1 &&
                        loop.dspHardware->task->loops.back().remaining > 0 &&
                        loop.dspHardware->task->loops.back().remaining < count,
                    "DSP loop did not retain its remaining iterations across a cooperative quantum.");
            for (unsigned i = 0; i < 4 && !loop.dspHardware->programHalted; ++i) NativeDspAdvanceTask(loop);
        }
        Require(loop.dspHardware->programHalted && loop.dspHardware->task->loops.empty() &&
                    loop.dspHardware->task->registers[0] == 0x40 + count &&
                    loop.dspHardware->dataRam[0x3f] == 0xa55a &&
                    loop.dspHardware->dataRam[0x40 + count] == 0xa55a &&
                    std::all_of(loop.dspHardware->dataRam.begin() + 0x40,
                                loop.dspHardware->dataRam.begin() + 0x40 + count,
                                [](uint16_t word) { return word == 0x55; }),
                "Compiled zero/single/quantum-spanning DSP loop wrote an incorrect range.");
    }
    auto arithmetic = stateFor({0x9900, 0xc100, 0x0021});
    arithmetic.executeDspTask = SyntheticDspArithmetic;
    NativeDspSetAccumulator(arithmetic, 0, -65536);
    NativeDspSetAccumulator(arithmetic, 1, -98305);
    arithmetic.dspHardware->task->registers[26] = 0xffff;
    NativeDspAdvanceTask(arithmetic);
    Require(NativeDspAccumulator(arithmetic, 1) == -2 &&
                (arithmetic.dspHardware->task->registers[19] & 0x3f) == 0x25,
            "Compiled arithmetic right shift or signed AX-high comparison is incorrect.");
    auto damagedTable = stateFor(SyntheticCommandWords);
    damagedTable.executeDspTask = SyntheticDspCommand;
    const auto tableRead = static_cast<uint16_t>(std::find(SyntheticCommandWords.begin(),
        SyntheticCommandWords.end(), 0x0213) - SyntheticCommandWords.begin());
    damagedTable.dspHardware->instructionPc = tableRead;
    damagedTable.dspHardware->task->registers[3] = SyntheticCommandWords[tableRead - 1];
    damagedTable.dspHardware->task->instructionRam[SyntheticCommandWords[tableRead - 1]] ^= 1;
    const auto tableRegisters = damagedTable.dspHardware->task->registers;
    Reject([&] { NativeDspAdvanceTask(damagedTable); }, "instruction-data");
    Require(damagedTable.dspHardware->task->registers == tableRegisters &&
                damagedTable.dspHardware->completedPrograms == 0,
            "Damaged instruction-data table changed register or completion state.");
    auto nonlinear = stateFor(SyntheticCommandWords);
    nonlinear.executeDspTask = SyntheticDspCommand;
    nonlinear.dspHardware->instructionPc = static_cast<uint16_t>(std::find(SyntheticCommandWords.begin(),
        SyntheticCommandWords.end(), 0x8970) - SyntheticCommandWords.begin());
    const auto nonlinearRegisters = nonlinear.dspHardware->task->registers;
    Reject([&] { NativeDspAdvanceTask(nonlinear); }, "linear addressing");
    Require(nonlinear.dspHardware->task->registers == nonlinearRegisters,
            "Unsupported parallel addressing changed the accumulator before failing.");
    auto buffers = stateFor(SyntheticBufferWords);
    buffers.memory.assign(512, 0xa5);
    buffers.executeDspTask = SyntheticDspBuffers;
    std::array<int64_t, 96> sums{};
    const std::array<int32_t, 8> values = {INT32_MIN, INT32_MAX, -1, 0, 1, 0x12345678, -1234567, 32768};
    for (unsigned i = 0; i < sums.size(); ++i) {
        const auto left = values[i % values.size()], right = values[(i / values.size()) % values.size()];
        sums[i] = int64_t(left) + right;
        buffers.dspHardware->dataRam[0x100 + 2 * i] = static_cast<uint16_t>(uint32_t(left) >> 16);
        buffers.dspHardware->dataRam[0x101 + 2 * i] = static_cast<uint16_t>(left);
        buffers.dspHardware->dataRam[0x500 + 2 * i] = static_cast<uint16_t>(uint32_t(right) >> 16);
        buffers.dspHardware->dataRam[0x501 + 2 * i] = static_cast<uint16_t>(right);
    }
    NativeDspAdvanceTask(buffers);
    Require(!buffers.dspHardware->programHalted && buffers.dspHardware->task->dataTransfers == 0,
            "DSP buffer processing failed to yield before its actual output DMA.");
    for (unsigned i = 0; i < 8 && !buffers.dspHardware->programHalted; ++i) NativeDspAdvanceTask(buffers);
    Require(buffers.dspHardware->programHalted && buffers.dspHardware->task->dataTransfers == 1 &&
                buffers.dspHardware->task->transferredBytes == 384 &&
                buffers.dspHardware->task->loops.empty() &&
                buffers.dspHardware->dataRam[0x8ff] == 0xa55a &&
                buffers.dspHardware->dataRam[0x9c0] == 0xa55a &&
                buffers.memory[0x3f] == 0xa5 && buffers.memory[0x1c0] == 0xa5,
            "DSP buffer loop/DMA wrote the wrong range or fabricated completion.");
    for (unsigned i = 0; i < sums.size(); ++i) {
        const auto sum = sums[i];
        const auto high = sum > INT32_MAX ? uint16_t(0x7fff) : sum < INT32_MIN ? uint16_t(0x8000)
            : static_cast<uint16_t>(uint64_t(sum) >> 16);
        Require(ppc_read_u16(buffers, 0x40 + i * 4) == static_cast<uint16_t>(sum) &&
                    ppc_read_u16(buffers, 0x42 + i * 4) == high,
                "DSP signed buffer addition, old-value parallel store or S40 saturation is incorrect.");
    }
    auto dual = stateFor({0x80f0, 0x80c0, 0x0021});
    dual.executeDspTask = SyntheticDspDualLoad;
    dual.dspHardware->task->registers[0] = 0x100;
    dual.dspHardware->task->registers[3] = 0x200;
    dual.dspHardware->task->registers[8] = dual.dspHardware->task->registers[11] = 0xffff;
    dual.dspHardware->dataRam[0x100] = 0x1234;
    dual.dspHardware->dataRam[0x101] = 0x5678;
    dual.dspHardware->dataRam[0x200] = 0xabcd;
    NativeDspAdvanceTask(dual);
    Require(dual.dspHardware->task->registers[26] == 0x1234 &&
                dual.dspHardware->task->registers[27] == 0x1234 &&
                dual.dspHardware->task->registers[24] == 0x5678 &&
                dual.dspHardware->task->registers[25] == 0x5678 &&
                dual.dspHardware->task->registers[0] == 0x102 &&
                dual.dspHardware->task->registers[3] == 0x202,
            "DSP same-bank dual-load bus behavior or address increments are incorrect.");
    for (bool badWrap : {false, true}) {
        auto invalidDual = stateFor({0x80f0, 0x80c0, 0x0021});
        invalidDual.executeDspTask = SyntheticDspDualLoad;
        invalidDual.dspHardware->task->registers[8] = 0xffff;
        invalidDual.dspHardware->task->registers[11] = badWrap ? 0 : 0xffff;
        invalidDual.dspHardware->task->registers[3] = badWrap ? 0 : 4096;
        const auto registers = invalidDual.dspHardware->task->registers;
        Reject([&] { NativeDspAdvanceTask(invalidDual); }, badWrap ? "linear" : "data RAM");
        Require(invalidDual.dspHardware->task->registers == registers,
                "Invalid DSP dual load partially changed its registers.");
    }
    for (int64_t sample : {INT64_C(0), INT64_C(0x7fff), INT64_C(-32768), INT64_C(-1)}) {
        auto interleave = stateFor({0xf07b, 0x197d, 0xf131, 0x8139, 0x7200, 0x0021});
        interleave.executeDspTask = SyntheticDspInterleave;
        auto& r = interleave.dspHardware->task->registers;
        r[1] = 0x40; r[3] = 0x80; r[9] = r[11] = 0xffff; r[25] = 0xffff; r[19] = 0x4000;
        interleave.dspHardware->dataRam[0x80] = 0x8001;
        interleave.dspHardware->dataRam[0x81] = 0x1234;
        NativeDspSetAccumulator(interleave, 0, sample);
        NativeDspSetAccumulator(interleave, 1, 0x1234);
        NativeDspAdvanceTask(interleave);
        Require(interleave.dspHardware->dataRam[0x40] == static_cast<uint16_t>(sample) &&
                    interleave.dspHardware->dataRam[0x41] == 0x7fff &&
                    r[1] == 0x42 && r[3] == 0x82 && NativeDspAccumulator(interleave, 0) == 65535 &&
                    NativeDspAccumulator(interleave, 1) == INT64_C(0x112340000),
                "DSP output interleave lost old values, truncation or unsigned pointer advancement.");
    }
    auto unknown = stateFor({0xffff});
    unknown.executeDspTask = UnsupportedDspTask;
    Reject([&] { NativeDspAdvanceTask(unknown); }, "translation at word 0");
    Require(unknown.dspHardware->executedInstructions == 0 &&
                unknown.dspHardware->completedPrograms == 0 && unknown.dspHardware->sentMail == 0,
            "Unsupported DSP task instruction fabricated execution or completion.");
    Reject([&] { RecompileDspTask({}, 0, "Task"); }, "Invalid");
    Reject([&] { RecompileDspTask({0x029f, 1}, 0, "Task"); }, "branch target");
    Reject([&] { RecompileDspTask({0x009e}, 0, "Task"); }, "Truncated");
    Reject([&] { RecompileDspTask({0x0021}, 0, "Task();"); }, "Invalid");
    Reject([&] { RecompileDspTask({0x0080, 0, 0x0021}, 0, "Task", {1}); }, "overlapping");
    Reject([&] { RecompileDspTask({0x0060, 0, 0x0021}, 0, "Task"); }, "loop range");
    Reject([&] { RecompileDspTask({0x8170, 0x0021}, 0, "Task"); }, "parallel accumulator write conflict");
}

void TestCompiledIntegerFloatStore() {
    for (bool absolute : {false, true}) {
        for (uint64_t bits : {UINT64_C(0), UINT64_C(0xfff8000000000001),
                              UINT64_C(0x4330000080000000), UINT64_C(0xffffffffffffffff)}) {
            PpcState state;
            state.memory.assign(32, 0xa5);
            state.fpr[3] = {bits, 0x123456789abcdef0};
            state.gpr[4] = absolute ? UINT32_MAX : 4;
            state.gpr[5] = absolute ? 8 : 4;
            state.cr = 0x12345678; state.fpscr = 0x87654321;
            const auto before = state;
            if (absolute) ppc_0000B830(state); else ppc_0000B820(state);
            Require(ppc_read_u32(state, 8) == static_cast<uint32_t>(bits) &&
                        state.memory[7] == 0xa5 && state.memory[12] == 0xa5 &&
                        state.fpr == before.fpr && state.cr == before.cr && state.fpscr == before.fpscr &&
                        std::equal(std::begin(state.gpr), std::end(state.gpr), std::begin(before.gpr)),
                    "stfiwx converted a raw integer payload or changed CPU state.");
            for (uint32_t address : {9u, 32u}) {
                auto bad = before;
                bad.gpr[5] = address - (absolute ? 0 : bad.gpr[4]);
                const auto memory = bad.memory;
                Reject([&] { if (absolute) ppc_0000B830(bad); else ppc_0000B820(bad); }, "");
                Require(bad.memory == memory && bad.fpr == before.fpr && bad.fpscr == before.fpscr,
                        "Invalid stfiwx partially committed its transfer.");
            }
        }
    }
    Reject([&] { RecompileFunction(0, {(31u << 26) | (983u << 1) | 1}); }, "record form");
}

void TestCompiledDspInitialization() {
    for (unsigned variant = 0; variant < 2; ++variant) {
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.dspControl = 4;
        state.dspHardware.emplace();
        auto& dsp = *state.dspHardware;
        dsp.dataRam.fill(0xa55au);
        state.executeDspInitialization = [variant](PpcState& guest) {
            if (variant == 0) SyntheticDspInitialization(*guest.dspHardware);
            else OtherSyntheticDspInitialization(*guest.dspHardware);
        };
        ppc_write_u16(state, 0xcc00500au, 0);
        const auto mail = variant == 0 ? 0x92345678u : 0xa3456789u;
        Require(dsp.fromDspMailbox == mail && dsp.programHalted &&
                    dsp.completedPrograms == 1 && dsp.sentMail == 1 &&
                    dsp.executedInstructions == 36 && state.dspControl == 4,
                "Compiled DSP program did not derive its mailbox from actual store instructions.");
        for (size_t index = 0; index < dsp.dataRam.size(); ++index) {
            Require(dsp.dataRam[index] == (index < 4 ? 0 : 0xa55au),
                    "Compiled DSP program lost its RAM store count or guard.");
        }
        const auto high = ppc_read_u16(state, 0xcc005004u);
        Require(high == mail >> 16 && dsp.fromDspMailbox == mail,
                "Reading DSP mailbox high consumed its pending flag.");
        Reject([&] { ppc_read_u16(static_cast<const PpcState&>(state), 0xcc005006u); },
               "consumption");
        Require(ppc_read_u16(state, 0xcc005006u) == static_cast<uint16_t>(mail) &&
                    dsp.fromDspMailbox == (mail & 0x7fffffffu) && dsp.consumedMail == 1,
                "DSP mailbox low did not consume exactly one real program message.");
        ppc_read_u16(state, 0xcc005006u);
        Require(dsp.consumedMail == 1, "Empty DSP mailbox read counted a nonexistent message.");
    }
    PpcDspHardware busy;
    busy.fromDspMailbox = 0x80000001u;
    Reject([&] { SyntheticDspInitialization(busy); }, "instruction budget exceeded");
    Require(busy.fromDspMailbox == 0x80000001u && busy.sentMail == 0 &&
                busy.completedPrograms == 0 && busy.executedInstructions == 65536,
            "Busy DSP mailbox was overwritten or its compiled wait bypassed the execution bound.");
    PpcDspHardware invalid;
    invalid.dataRam.fill(0xa55au);
    Reject([&] { OutOfRangeDspInitialization(invalid); }, "proven region");
    Reject([&] { WrappedDspInitialization(invalid); }, "proven region");
    Require(invalid.sentMail == 0 && invalid.completedPrograms == 0 &&
                std::all_of(invalid.dataRam.begin(), invalid.dataRam.end(),
                            [](uint16_t word) { return word == 0xa55au; }),
            "Invalid DSP ROM elimination fabricated data or ran later program effects.");
    for (size_t length = 0; length + 1 < SyntheticDspWords.size(); ++length) {
        Reject([&] {
            RecompileDspInitialization({SyntheticDspWords.begin(), SyntheticDspWords.begin() + length},
                                      "TruncatedDsp");
        }, "DSP");
    }
    auto mutated = SyntheticDspWords;
    const auto loop = std::find(mutated.begin(), mutated.end(), 0x0064u) - mutated.begin();
    mutated[loop + 4] = 0x1c1eu;
    Reject([&] { RecompileDspInitialization(mutated, "LiveRom"); }, "data flow");
    mutated = SyntheticDspWords;
    mutated[loop + 3] = 0x1c1eu;
    Reject([&] { RecompileDspInitialization(mutated, "ObservedRom"); }, "data flow");
    mutated = SyntheticDspWords;
    mutated[loop + 1] -= 1;
    Reject([&] { RecompileDspInitialization(mutated, "WrongLoopEnd"); }, "data flow");
    mutated = SyntheticDspWords;
    mutated[7] = 0x0085u;
    Reject([&] { RecompileDspInitialization(mutated, "UndefinedCounter"); }, "data flow");
    mutated = SyntheticDspWords;
    mutated[8] = 0x1001u;
    const auto rangeChecked = RecompileDspInitialization(mutated, "RangeCheckedDsp");
    Require(rangeChecked.find("proven region") != std::string::npos,
            "DSP ROM elimination did not emit runtime bounds checks.");
    Reject([&] { RecompileDspInitialization(SyntheticDspWords, "invalid-name"); }, "function name");
}

void TestSiTransactionsAndPendingInterrupt() {
    for (unsigned channel = 0; channel < 4; ++channel) {
        for (unsigned outputLength : {1u, 3u, 4u, 127u, 128u}) {
            for (unsigned inputLength : {1u, 3u, 4u, 127u, 128u}) {
                for (uint32_t error : {0u, 1u, 2u, 4u, 8u, 15u}) {
                    PpcState state;
                    state.memoryMap = PpcMemoryMap::GameCubeRam;
                    state.serialInterface.emplace();
                    auto& serial = *state.serialInterface;
                    serial.communication = 0xb0000000u;
                    serial.status = 0x01010101u;
                    for (unsigned index = 0; index < 32; ++index) {
                        serial.buffer[index] = 0x12345678u + index;
                    }
                    const auto before = serial.buffer;
                    unsigned transfers = 0;
                    state.transferSi = [&](const SiTransferRequest& request) {
                        Require(request.channel == channel && request.inputLength == inputLength &&
                                    request.output.size() == outputLength && transfers++ == 0,
                                "SI channel, zero-encoded 128-byte length or transaction count differs.");
                        for (unsigned index = 0; index < outputLength; ++index) {
                            Require(request.output[index] ==
                                        static_cast<uint8_t>(before[index / 4] >> (24u - index % 4u * 8u)),
                                    "SI output byte order differs.");
                        }
                        return SiTransferResult{error == 0 ? std::vector<uint8_t>(inputLength, 0xa5) :
                                                            std::vector<uint8_t>{}, error};
                    };
                    const uint32_t value = 0xc0000001u | (channel << 1) |
                        ((outputLength & 127u) << 16) | ((inputLength & 127u) << 8);
                    ppc_write_u32(state, 0xcc006434u, value);
                    Require(transfers == 1 &&
                                serial.communication == ((value & ~1u) | 0x10000000u |
                                                         (error != 0 ? 0x20000000u : 0u)) &&
                                serial.status == (0x01010101u | (error << ((3u - channel) * 8u))),
                            "SI completion, COMERR, channel status or interrupt mask differs.");
                    for (unsigned index = 0; index < 128; ++index) {
                        const auto actual = static_cast<uint8_t>(serial.buffer[index / 4] >>
                                                               (24u - index % 4u * 8u));
                        const auto expected = error == 0 && index < inputLength ? uint8_t{0xa5} :
                            static_cast<uint8_t>(before[index / 4] >> (24u - index % 4u * 8u));
                        Require(actual == expected, "SI completion overwrote unreceived communication RAM.");
                    }
                }
            }
        }
    }
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.serialInterface.emplace();
    state.serialInterface->buffer.fill(0x12345678u);
    for (const auto& result : {SiTransferResult{{}, 0}, SiTransferResult{{1, 2, 3}, 16},
                               SiTransferResult{{1, 2, 3}, 8}, SiTransferResult{{1, 2}, 0}}) {
        state.transferSi = [result](const SiTransferRequest&) { return result; };
        Reject([&] { ppc_write_u32(state, 0xcc006434u, 0xc0010301u); }, "Invalid native SI");
        Require(state.serialInterface->communication == 0 && state.serialInterface->status == 0 &&
                    state.serialInterface->buffer.front() == 0x12345678u,
                "Invalid SI backend result partially mutated hardware state.");
    }
    state.transferSi = [](const SiTransferRequest&) -> SiTransferResult {
        throw std::runtime_error("SI backend failure");
    };
    Reject([&] { ppc_write_u32(state, 0xcc006434u, 0xc0010301u); }, "SI backend failure");
    Require(state.serialInterface->communication == 0 && state.serialInterface->status == 0,
            "Throwing SI backend fabricated completion.");
    state.transferSi = TransferDisconnectedSi;
    ppc_write_u32(state, 0xcc006434u, 0xc0010301u);
    Require(state.serialInterface->communication == 0xe0010300u &&
                state.serialInterface->status == 0x08000000u &&
                state.serialInterface->buffer.front() == 0x12345678u,
            "Disconnected SI port did not signal no-response without inventing input.");
    state.externalInterruptsEnabled = false;
    state.piInterruptMask = 0xf8u;
    Reject([&] { ppc_write_msr(state, 0x8000u); }, "guest interrupt delivery");
    Require(!state.externalInterruptsEnabled && state.msr == 0,
            "Missing interrupt delivery silently enabled EE or lost the SI interrupt.");
    state.externalInterruptsEnabled = true;
    NativeDispatcher dispatcher;
    Reject([&] { dispatcher.Execute(state, 0x1000u); }, "guest interrupt delivery");
    Require(!state.callGuest, "Pending SI interrupt failure leaked dispatcher callbacks.");
    ppc_write_u32(state, 0xcc006434u, 0xc0010300u);
    ppc_write_msr(state, 0x8000u);
    Require(state.externalInterruptsEnabled &&
                state.serialInterface->communication == 0x60010300u,
            "SI transfer-complete acknowledgement did not release the pending interrupt.");
    for (auto request : {SiTransferRequest{4, {0}, 3}, SiTransferRequest{0, {}, 3},
                          SiTransferRequest{0, {0}, 0}, SiTransferRequest{0, {0}, 129}}) {
        Reject([&] { TransferDisconnectedSi(request); }, "Invalid native SI");
    }
}

void TestDiscDmaAndDeadline() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(4096, 0xa5);
    state.discInterface.emplace();
    state.externalInterrupt.emplace();
    state.piInterruptMask = 0;
    auto& di = *state.discInterface;
    for (uint32_t initial = 0; initial < 128; initial += 2) {
        for (uint32_t value = 0; value < 128; value += 2) {
            di.status = initial;
            ppc_write_u32(state, 0xcc006000u, value);
            Require(ppc_read_u32(state, 0xcc006000u) ==
                        ((initial & 0x54u & ~(value & 0x54u)) | (value & 0x2au)),
                    "DI status masks/W1C acknowledgement differ.");
        }
    }
    for (uint32_t initial = 0; initial < 8; ++initial) {
        for (uint32_t value : {0u, 2u, 4u, 6u}) {
            di.cover = initial;
            ppc_write_u32(state, 0xcc006004u, value);
            Require(ppc_read_u32(state, 0xcc006004u) ==
                        ((initial & 5u & ~(value & 4u)) | (value & 2u)),
                    "DI cover status/mask/W1C acknowledgement differ.");
        }
    }
    di.cover = 0;
    di.status = 0x2au;
    ppc_write_u32(state, 0xcc006008u, 0x12000000u);
    ppc_write_u32(state, 0xcc00600cu, 0);
    ppc_write_u32(state, 0xcc006010u, 32);
    ppc_write_u32(state, 0xcc006014u, 0x80000040u);
    ppc_write_u32(state, 0xcc006018u, 32);
    const auto memory = state.memory;
    Reject([&] { ppc_write_u32(state, 0xcc00601cu, 3); }, "requires a native disc");
    Require(di.control == 0 && di.dmaLength == 32 && di.completedTransfers == 0 &&
                di.status == 0x2au && state.memory == memory,
            "Missing DI backend fabricated completion or DMA data.");
    for (unsigned size : {0u, 31u, 33u}) {
        state.transferDisc = [size](const std::array<uint32_t, 3>&, uint32_t) {
            return std::vector<uint8_t>(size);
        };
        Reject([&] { ppc_write_u32(state, 0xcc00601cu, 3); }, "invalid DMA length");
        Require(di.dmaLength == 32 && di.completedTransfers == 0 && state.memory == memory,
                "Invalid DI response partially changed DMA state or RAM.");
    }
    state.transferDisc = [](const std::array<uint32_t, 3>&, uint32_t) -> std::vector<uint8_t> {
        throw std::runtime_error("disc read failed");
    };
    Reject([&] { ppc_write_u32(state, 0xcc00601cu, 3); }, "disc read failed");
    Require(state.memory == memory && di.completedTransfers == 0 && di.status == 0x2au,
            "Throwing DI backend committed successful status or bytes.");
    unsigned calls = 0;
    state.transferDisc = [&](const std::array<uint32_t, 3>& command, uint32_t length) {
        Require(command == std::array<uint32_t, 3>{0x12000000u, 0, 32} && length == 32,
                "DI command words or DMA request length differ.");
        ++calls;
        return std::vector<uint8_t>(32, 0x3c);
    };
    for (auto invalid : {std::pair<uint32_t, uint32_t>{0xcc006014u, 0x80000041u},
                         {0xcc006014u, 0x81800000u}, {0xcc006018u, 31u},
                         {0xcc006018u, kGameCubeRamSize + 32u}, {0xcc00601cu, 1u},
                         {0xcc00601cu, 7u}, {0xcc006000u, 1u}, {0xcc006004u, 8u}}) {
        Reject([&] { ppc_write_u32(state, invalid.first, invalid.second); }, "");
        Require(state.memory == memory && di.dmaLength == 32 && di.dmaAddress == 0x80000040u &&
                    di.completedTransfers == 0 && calls == 0,
                "Invalid DI register write mutated DMA or called its backend.");
    }
    ppc_write_u32(state, 0xcc006014u, 0x80000fe0u);
    ppc_write_u32(state, 0xcc006018u, 64);
    Reject([&] { ppc_write_u32(state, 0xcc00601cu, 3); }, "");
    Require(calls == 0 && state.memory == memory, "Out-of-bounds DI DMA reached the backend.");
    ppc_write_u32(state, 0xcc006014u, 0x80000040u);
    ppc_write_u32(state, 0xcc006018u, 32);
    ppc_write_u32(state, 0xcc00601cu, 3);
    Require(calls == 1 && di.completedTransfers == 1 && di.dmaLength == 0 &&
                di.dmaAddress == 0x80000060u && di.control == 2 && di.status == 0x3au &&
                ppc_read_u32(state, 0xcc003000u) == 4 && !ppc_external_interrupt_pending(state),
            "DI DMA completion, source cause or PI masking differs.");
    for (size_t index = 0; index < state.memory.size(); ++index) {
        Require(state.memory[index] == (index >= 64 && index < 96 ? 0x3c : 0xa5),
                "DI DMA bytes or destination guards differ.");
    }
    state.serialInterface.emplace();
    ppc_write_u32(state, 0xcc003004u, 0xfcu);
    Require(ppc_external_interrupt_pending(state), "Unmasked DI completion was not retained.");
    state.externalInterruptsEnabled = true;
    Reject([&] { ppc_check_pending_interrupts(state, true); }, "guest interrupt delivery");
    ppc_write_u32(state, 0xcc006000u, 0x3au);
    Require(!ppc_external_interrupt_pending(state) && di.status == 0x2au,
            "DI W1C acknowledgement did not release its pending source.");
    uint64_t ticks = 100;
    state.readTimeBase = [&] { return ticks; };
    Reject([&] { ppc_write_decrementer(state, 10); }, "missing timer profile");
    state.decrementer.emplace();
    ppc_write_decrementer(state, 10);
    Require(state.decrementer->deadline == 111 && state.decrementer->writes == 1,
            "DEC did not use the actual time-base clock.");
    ticks = 110;
    ppc_check_pending_interrupts(state, true);
    ticks = 111;
    ppc_check_pending_interrupts(state, false);
    Reject([&] { ppc_check_pending_interrupts(state, true); }, "timer exception delivery");
    ppc_write_decrementer(state, 0);
    ppc_check_pending_interrupts(state, true);
    ++ticks;
    Reject([&] { ppc_check_pending_interrupts(state, true); }, "timer exception delivery");
    Reject([&] { ppc_write_decrementer(state, 0x80000000u); }, "Unsupported native decrementer");
    ticks = UINT64_MAX;
    Reject([&] { ppc_write_decrementer(state, 1); }, "deadline overflow");
    Require(state.decrementer->deadline == 112 && state.decrementer->writes == 2,
            "Rejected DEC write changed its existing deadline.");
}

void TestNativeSramAndDma() {
    for (unsigned channel = 0; channel < 3; ++channel) {
        for (unsigned device = 0; device < 3; ++device) {
            for (unsigned direction = 0; direction < 2; ++direction) {
                for (unsigned length : {32u, 64u, 128u}) {
                    PpcState dmaState;
                    dmaState.memoryMap = PpcMemoryMap::GameCubeRam;
                    dmaState.memory.resize(256, 0xa5);
                    dmaState.exiDmaEnabled = true;
                    dmaState.exiStatus[channel] = 0x80u << device;
                    dmaState.exiTransferControl[channel] = 0;
                    unsigned transfers = 0;
                    dmaState.transferExiDma = [&](const ExiDmaRequest& request) {
                        Require(request.channel == channel && request.device == device &&
                                    request.direction == direction && request.length == length && transfers++ == 0 &&
                                    request.output == (direction == 1 ? std::vector<uint8_t>(length, 0xa5) :
                                                                       std::vector<uint8_t>{}),
                                "EXI DMA channel/device/direction/length or write payload differs.");
                        return direction == 0 ? std::vector<uint8_t>(length, 0x3c) : std::vector<uint8_t>{};
                    };
                    const auto base = 0xcc006800u + channel * 0x14u;
                    ppc_write_u32(dmaState, base + 4, 64);
                    ppc_write_u32(dmaState, base + 8, length);
                    ppc_write_u32(dmaState, base + 12, 3u | (direction << 2));
                    Require(transfers == 1 && dmaState.exiStatus[channel] == ((0x80u << device) | 8u) &&
                                dmaState.exiTransferControl[channel] == (2u | (direction << 2)),
                            "EXI DMA completion or selected-device status differs.");
                    for (unsigned index = 0; index < dmaState.memory.size(); ++index) {
                        const auto expected = direction == 0 && index >= 64 && index < 64 + length ? 0x3cu : 0xa5u;
                        Require(dmaState.memory[index] == expected, "EXI DMA direction or RAM bounds differ.");
                    }
                }
            }
        }
    }
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(256, 0xa5);
    state.exiStatus = {0u, 0u, 0u};
    state.exiTransferControl = {0u, 0u, 0u};
    state.exiImmediateEnabled = true;
    state.exiInitializationEnabled = true;
    const auto sram = NewNativeSram();
    Require(sram[0] == 0 && sram[1] == 0x2c && sram[2] == 0xff && sram[3] == 0xd0 &&
                sram[0x12] == 0 && sram[0x13] == 0x2c,
            "Native SRAM checksum, language or NTSC/stereo configuration differs.");
    BindNativeSram(state, sram);
    ppc_write_u32(state, 0xcc006800u, 0x2130u);
    ppc_write_u32(state, 0xcc006810u, 0x20000100u);
    ppc_write_u32(state, 0xcc00680cu, 0x35u);
    Require(state.exiTransferControl[0] == 0x34u, "SRAM read command did not complete.");
    ppc_write_u32(state, 0xcc006804u, 64);
    ppc_write_u32(state, 0xcc006808u, 64);
    Require(ppc_read_u32(state, 0xcc006804u) == 64 && ppc_read_u32(state, 0xcc006808u) == 64,
            "EXI DMA register readback differs.");
    ppc_write_u32(state, 0xcc00680cu, 3);
    Require(state.exiTransferControl[0] == 2 && state.exiStatus[0] == 0x2138u,
            "SRAM DMA did not complete with TCINT and retained selection.");
    for (unsigned index = 0; index < state.memory.size(); ++index) {
        Require(state.memory[index] == (index >= 64 && index < 128 ? sram[index - 64] : 0xa5u),
                "SRAM DMA bytes or surrounding memory changed unexpectedly.");
    }
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 3); }, "SRAM DMA");
    ppc_write_u32(state, 0xcc006800u, 0x2030u);
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 3); }, "selected device");
    ppc_write_u32(state, 0xcc006800u, 0x2130u);
    ppc_write_u32(state, 0xcc006810u, 0x20000000u);
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 0x35u); }, "SRAM/RTC/ROM command");
    ppc_write_u32(state, 0xcc006810u, 0xa0000100u);
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 0x35u); }, "SRAM/RTC/ROM command");
    ppc_write_u32(state, 0xcc006810u, 0x20000100u);
    ppc_write_u32(state, 0xcc00680cu, 0x35u);
    const auto before = state.memory;
    const auto control = state.exiTransferControl[0];
    const auto status = state.exiStatus[0];
    const auto dma = state.transferExiDma;
    state.transferExiDma = {};
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 3); }, "peripheral backend");
    state.transferExiDma = [](const ExiDmaRequest&) -> std::vector<uint8_t> {
        throw std::runtime_error("SRAM storage unavailable");
    };
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 3); }, "storage unavailable");
    state.transferExiDma = [](const ExiDmaRequest&) { return std::vector<uint8_t>(63, 0); };
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 3); }, "result length");
    Require(state.memory == before && state.exiTransferControl[0] == control && state.exiStatus[0] == status,
            "Failed EXI DMA partially committed RAM, control or completion.");
    state.transferExiDma = dma;
    ppc_write_u32(state, 0xcc006804u, 0x100u);
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 3); }, "outside guest memory");
    Require(state.exiTransferControl[0] == control && state.memory == before,
            "Out-of-range EXI DMA consumed its command or changed hardware.");
    ppc_write_u32(state, 0xcc006804u, 64);
    for (auto value : {1u, 31u, 0x04000000u, UINT32_MAX}) {
        Reject([&] { ppc_write_u32(state, 0xcc006804u, value); }, "aligned");
        Reject([&] { ppc_write_u32(state, 0xcc006808u, value); }, "aligned");
    }
    ppc_write_u32(state, 0xcc006808u, 0);
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 3); }, "nonzero");
    ppc_write_u32(state, 0xcc006808u, 64);
    Reject([&] { ppc_write_u32(state, 0xcc00680cu, 11); }, "transfer mode");
    ppc_write_u32(state, 0xcc00680cu, 3);
    state.gpr[4] = 64;
    state.gpr[5] = 1;
    state.cr = 0x12345678u;
    state.xer = 0xe0000000u;
    const auto cacheMemory = state.memory;
    ppc_0000B5A0(state);
    Require(state.memory == cacheMemory && state.cr == 0x12345678u && state.xer == 0xe0000000u,
            "Compiled dcbi changed coherent RAM or CPU flags.");
}

void TestCompiledInterruptRegisters() {
    PpcState state;
    state.cr = 0x12345678u;
    ppc_0000B600(state);
    Require(state.gpr[3] == state.cr, "Compiled mfcr did not read every CR field.");
    state.gpr[3] = 0xabcdef01u;
    ppc_0000B610(state);
    Require(state.cr == 0x1b3de608u, "Compiled mtcrf did not preserve unselected fields.");
    for (uint32_t value : {0u, 0x7fffffffu, UINT32_MAX}) {
        for (uint32_t carry : {0u, 0x20000000u}) {
            state.gpr[4] = value;
            state.xer = 0xc0000000u | carry;
            ppc_0000B620(state);
            const uint64_t expected = static_cast<uint64_t>(value) + (carry != 0 ? 1u : 0u);
            Require(state.gpr[3] == static_cast<uint32_t>(expected) &&
                        state.xer == (0xc0000000u | (expected > UINT32_MAX ? 0x20000000u : 0u)) &&
                        (state.cr >> 28) == (state.gpr[3] == 0 ? 3u :
                                            (state.gpr[3] & 0x80000000u) != 0 ? 9u : 5u),
                    "Compiled addze lost carry, signed CR0 or XER SO/OV.");
        }
    }
    for (auto function : {ppc_0000B630, ppc_0000B640, ppc_0000B650}) {
        state.gpr[3] = 0x82345678u;
        state.gpr[4] = 0;
        function(state);
        Require(state.gpr[4] == state.gpr[3], "Compiled exception SPR round trip differs.");
    }
    Require(state.srr0 == 0x82345678u && state.srr1 == 0x82345678u &&
                state.sprg0 == 0x82345678u, "Compiled exception SPR targets differ.");
}

void TestThreadContextReturn() {
    const auto create = [] {
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.memory.resize(0x4000);
        state.ramBats.emplace();
        state.msr = 0x30;
        state.externalInterruptsEnabled = false;
        state.lr = 0xfffffffcu;
        state.gpr[1] = 0x80003f00u;
        state.srr0 = 0x80003000u;
        state.srr1 = 0x8032u;
        ppc_write_u32(state, 0x800000d4u, 0x80003810u);
        ppc_write_u32(state, 0x80003810u + 4, state.gpr[1]);
        ppc_write_u32(state, 0x80003810u + 0x198, state.srr0);
        ppc_write_u32(state, 0x80003810u + 0x19c, state.srr1);
        for (uint32_t address : {0x1000u, 0x2000u, 0x2100u, 0x3000u}) {
            ppc_write_u32(state, address, 0x60000000u);
        }
        return state;
    };
    auto state = create();
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, {
        {0x80001000u, [](PpcState& guest) {
            guest.lr = 0x80001004u;
            guest.callGuest(0x80002000u, guest);
            ++guest.gpr[30];
        }, {0x60000000u}},
        {0x80002000u, [](PpcState& guest) {
            guest.lr = 0x80002004u;
            guest.callGuest(0x80002100u, guest);
            ++guest.gpr[30];
        }, {0x60000000u}},
        {0x80002100u, [](PpcState& guest) { ppc_0000B5C0(guest); }, {0x60000000u}},
        {0x80003000u, [](PpcState& guest) {
            Require(guest.lr == 0x80002004u && guest.gpr[1] == 0x80003f00u &&
                        ppc_read_msr(guest) == guest.srr1,
                    "Thread context return corrupted saved CPU state.");
            ++guest.gpr[31];
            guest.pc = 0xfffffffcu;
        }, {0x60000000u}}
    });
    dispatcher.Execute(state, 0x80001000u);
    Require(state.threadContextReturns == 1 && state.gpr[30] == 0 && state.gpr[31] == 1 &&
                !state.callGuest && !state.callException && state.pc == 0xfffffffcu,
            "Compiled thread rfi did not abandon the guest call chain and resume the original context.");
    for (uint32_t vector : {0x500u, 0x900u}) {
        for (uint32_t mode : {0x8032u, 0x9032u}) {
            auto interrupted = create();
            interrupted.srr1 = mode;
            ppc_write_u32(interrupted, 0x80003810u + 0x19c, mode);
            interrupted.externalInterrupt.emplace();
            auto& interrupt = *interrupted.externalInterrupt;
            interrupt.active = true;
            interrupt.inHandler = true;
            interrupt.resume = 0x80001000u;
            interrupt.savedMsr = 0x8032u;
            interrupt.vector = vector;
            if (vector == 0x900u) {
                auto missingTimer = interrupted;
                Reject([&] { dispatcher.Execute(missingTimer, 0x80001000u); },
                       "Missing active decrementer context");
                Require(missingTimer.externalInterrupt->active && missingTimer.externalInterrupt->inHandler &&
                            missingTimer.threadContextReturns == 0 && ppc_read_msr(missingTimer) == 0x30 &&
                            !missingTimer.callGuest && !missingTimer.callException,
                        "Missing DEC state fabricated a completed interrupt/thread return.");
                interrupted.decrementer.emplace();
            }
            dispatcher.Execute(interrupted, 0x80001000u);
            Require(interrupted.threadContextReturns == 1 && interrupted.gpr[30] == 0 &&
                        interrupted.gpr[31] == 1 && !interrupt.active &&
                        (vector == 0x900u ? interrupted.decrementer->returned : interrupt.returned) == 1 &&
                        ppc_read_msr(interrupted) == mode &&
                        !interrupted.callGuest && !interrupted.callException,
                    "Interrupt scheduler return did not unwind to the validated new thread.");
        }
    }
    auto limited = create();
    Reject([&] { dispatcher.Execute(limited, 0x80001000u, 3); }, "budget exceeded");
    Require(limited.threadContextReturns == 1 && limited.gpr[31] == 0 &&
                !limited.callGuest && !limited.callException,
            "Thread context unwinding reset the shared dispatch budget or leaked callbacks.");
    for (uint32_t offset : {4u, 0x198u, 0x19cu}) {
        auto invalid = create();
        ppc_write_u32(invalid, 0x80003810u + offset, 0);
        Reject([&] { dispatcher.Execute(invalid, 0x80001000u); }, "original current OS context");
        Require(invalid.threadContextReturns == 0 && ppc_read_msr(invalid) == 0x30 &&
                    !invalid.callGuest && !invalid.callException,
                "Rejected thread context mutated MSR, counted success or leaked dispatcher ownership.");
        invalid.externalInterrupt.emplace();
        invalid.externalInterrupt->active = true;
        invalid.externalInterrupt->inHandler = true;
        invalid.externalInterrupt->resume = 0x80001000u;
        invalid.externalInterrupt->savedMsr = 0x8032u;
        Reject([&] { dispatcher.Execute(invalid, 0x80001000u); }, "original current OS context");
        Require(invalid.externalInterrupt->returned == 0 && invalid.threadContextReturns == 0 &&
                    ppc_read_msr(invalid) == 0x30 && !invalid.callGuest && !invalid.callException,
                "Invalid interrupt thread context fabricated a return or changed MSR.");
    }
    auto changed = create();
    ppc_write_u32(changed, 0x80003000u, 0);
    Reject([&] { dispatcher.Execute(changed, 0x80001000u); }, "code mismatch");
    Require(!changed.callGuest && !changed.callException,
            "Thread continuation bypassed translation integrity checks.");
}

void TestExternalInterruptDelivery() {
    const auto create = [] {
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.memory.resize(0x4000);
        state.externalInterrupt.emplace();
        state.serialInterface.emplace();
        state.serialInterface->communication = 0xc0000000u;
        state.piInterruptMask = 8;
        state.msr = 0x32u;
        state.externalInterruptsEnabled = true;
        state.lr = 0xfffffffcu;
        for (uint32_t address : {0x500u, 0x1000u, 0x2000u, 0x2100u, 0x3000u}) {
            ppc_write_u32(state, address, 0x60000000u);
        }
        return state;
    };
    auto initial = create();
    NativeDispatcher dispatcher;
    dispatcher.Register(0, initial, {
        {0x500u, [](PpcState& state) {
            Require(state.externalInterrupt->active && !state.externalInterrupt->inHandler &&
                        !state.externalInterruptsEnabled && state.srr0 == 0x80001000u &&
                        state.srr1 == 0x8032u && state.lr == 0xfffffffcu,
                    "External interrupt did not capture the actual guest PC/MSR or changed LR.");
            state.srr0 = 0x80002000u;
            state.srr1 = 0x30u;
            ppc_return_from_system_call(state);
        }, {0x60000000u}, 0x3000u, {0x60000000u}},
        {0x80002000u, [](PpcState& state) {
            state.lr = 0x80002004u;
            state.callGuest(0x80002100u, state);
            ++state.gpr[30];
        }, {0x60000000u}},
        {0x80002100u, [](PpcState& state) {
            ppc_write_u32(state, 0xcc006434u, 0xc0000000u);
            state.srr0 = state.externalInterrupt->resume;
            state.srr1 = state.externalInterrupt->savedMsr;
            state.lr = 0xfffffffcu;
            ppc_return_from_system_call(state);
        }, {0x60000000u}},
        {0x80001000u, [](PpcState& state) { ++state.gpr[31]; state.pc = state.lr; },
         {0x60000000u}}
    });
    dispatcher.Execute(initial, 0x80001000u);
    Require(initial.gpr[31] == 1 && initial.gpr[30] == 0 && initial.externalInterrupt->delivered == 1 &&
                initial.externalInterrupt->returned == 1 && !initial.externalInterrupt->active &&
                ppc_read_msr(initial) == 0x8032u && initial.pc == 0xfffffffcu &&
                !initial.callGuest && !initial.callException,
            "Interrupt rfi did not unwind nested guest calls or restore dispatcher ownership.");
    for (unsigned limit : {1u, 2u}) {
        auto limited = create();
        Reject([&] { dispatcher.Execute(limited, 0x80001000u, limit); }, "budget exceeded");
        Require(!limited.callGuest && !limited.callException && limited.externalInterrupt->returned == 0,
                "Exception budget failure leaked callbacks or fabricated context restoration.");
    }
    for (unsigned depth : {1u, 2u}) {
        auto shallow = create();
        Reject([&] { dispatcher.Execute(shallow, 0x80001000u, 100, depth); }, "call-depth");
        Require(!shallow.callGuest && !shallow.callException,
                "External exception bypassed the shared call-depth limit.");
    }
    auto damaged = create();
    ppc_write_u32(damaged, 0x500u, 0);
    Reject([&] { dispatcher.Execute(damaged, 0x80001000u); }, "code mismatch");
    auto missing = create();
    missing.externalInterrupt.reset();
    Reject([&] { dispatcher.Execute(missing, 0x80001000u); }, "guest interrupt delivery");
    auto masked = create();
    masked.piInterruptMask = 0;
    Require(ppc_read_u32(masked, 0xcc003000u) == 8 && !ppc_si_interrupt_pending(masked),
            "PI source cause incorrectly depends on the PI interrupt mask.");
    dispatcher.Execute(masked, 0x80001000u);
    Require(masked.externalInterrupt->delivered == 0 &&
                masked.serialInterface->communication == 0xc0000000u,
            "Masked SI source was delivered or silently cleared.");
    auto invalid = create();
    invalid.externalInterrupt->active = true;
    invalid.externalInterrupt->inHandler = true;
    invalid.externalInterruptsEnabled = false;
    invalid.externalInterrupt->resume = 0x80001000u;
    invalid.externalInterrupt->savedMsr = 0x8032u;
    invalid.externalInterrupt->suspended.resize(3);
    invalid.srr0 = 0x80001004u;
    invalid.srr1 = 0x8032u;
    Reject([&] { ppc_return_from_system_call(invalid); }, "unnested translated context");
    Reject([&] { ppc_write_msr(invalid, 0x8032u); }, "guest interrupt delivery");
    Reject([&] { dispatcher.Register(1, invalid,
        {{0x80003004u, ppc_0000B5C0, {0x4c000064u}, 0, {0x60000000u}}}); },
        "installation words");
    Reject([&] { dispatcher.Register(1, invalid,
        {{0x80003004u, ppc_0000B5C0, {0x4c000064u}, 0x3000u,
          {0x60000000u, 0x60000000u}}}); }, "installation words");
    ppc_write_u32(invalid, 0x3000u, 0);
    Reject([&] { dispatcher.Register(1, invalid,
        {{0x80003004u, ppc_0000B5C0, {0x4c000064u}, 0x3000u, {0x60000000u}}}); },
        "code mismatch");
}

void TestNestedExternalInterruptDelivery() {
    const auto create = [] {
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.memory.resize(0x4000);
        state.externalInterrupt.emplace();
        state.serialInterface.emplace();
        state.serialInterface->communication = 0xc0000000u;
        state.piInterruptMask = 8;
        state.msr = 0x32;
        state.externalInterruptsEnabled = true;
        state.lr = 0xfffffffcu;
        for (uint32_t address : {0x500u, 0x1000u, 0x2000u, 0x2100u}) {
            ppc_write_u32(state, address, 0x60000000);
        }
        return state;
    };
    auto baseline = create();
    NativeDispatcher dispatcher;
    dispatcher.Register(0, baseline, {
        {0x500, [](PpcState& state) {
            const auto& interrupt = *state.externalInterrupt;
            const bool nested = !interrupt.suspended.empty();
            Require(interrupt.active && !interrupt.inHandler && !state.externalInterruptsEnabled &&
                        state.srr0 == (nested ? 0x80002100u : 0x80001000u) &&
                        state.srr1 == 0x8032u,
                    "Nested interrupt did not preserve its actual interrupted PC/MSR.");
            state.srr0 = 0x80002000; state.srr1 = 0x30;
            ppc_return_from_system_call(state);
        }, {0x60000000}},
        {0x80002000, [](PpcState& state) {
            ppc_write_u32(state, 0xcc006434, 0xc0000000);
            ++state.gpr[30];
            if (state.externalInterrupt->suspended.size() + 1 < state.gpr[29]) {
                state.serialInterface->communication = 0xc0000000;
                ppc_write_msr(state, 0x8032);
            }
            state.pc = 0x80002100;
        }, {0x60000000}},
        {0x80002100, [](PpcState& state) {
            auto& interrupt = *state.externalInterrupt;
            Require(interrupt.active && interrupt.inHandler &&
                        interrupt.resume == (interrupt.suspended.empty() ? 0x80001000u : 0x80002100u) &&
                        interrupt.savedMsr == 0x8032,
                    "Nested interrupt return replaced the parent resume/MSR metadata.");
            ppc_write_msr(state, 0x32);
            ++state.gpr[28];
            state.srr0 = interrupt.resume; state.srr1 = interrupt.savedMsr;
            ppc_return_from_system_call(state);
        }, {0x60000000}},
        {0x80001000, [](PpcState& state) { ++state.gpr[31]; state.pc = state.lr; }, {0x60000000}}
    });
    for (unsigned levels : {1u, 2u, 4u}) {
        auto state = create();
        state.gpr[29] = levels;
        dispatcher.Execute(state, 0x80001000);
        Require(state.gpr[30] == levels && state.gpr[28] == levels && state.gpr[31] == 1 &&
                    state.externalInterrupt->delivered == levels && state.externalInterrupt->returned == levels &&
                    !state.externalInterrupt->active && state.externalInterrupt->suspended.empty() &&
                    ppc_read_msr(state) == 0x8032 && state.pc == 0xfffffffcu &&
                    !state.callException && !state.callGuest,
                "Nested interrupt chain did not return through every frame exactly once.");
    }
    auto overflow = create();
    overflow.gpr[29] = 5;
    Reject([&] { dispatcher.Execute(overflow, 0x80001000); }, "guest interrupt delivery");
    Require(overflow.externalInterrupt->delivered == 4 && overflow.externalInterrupt->returned == 0 &&
                overflow.externalInterrupt->suspended.size() == 3 && !overflow.callException && !overflow.callGuest,
            "Interrupt nesting overflow lost active frames or fabricated a return.");
    for (bool syscall : {false, true}) {
        auto invalid = create();
        invalid.externalInterrupt->active = true;
        invalid.externalInterrupt->inHandler = syscall;
        invalid.systemCallActive = syscall;
        invalid.callException = [](uint32_t, uint32_t, PpcState&) {
            throw std::runtime_error("Invalid nested delivery reached its exception callback.");
        };
        const auto before = *invalid.externalInterrupt;
        Reject([&] { ppc_deliver_external_interrupt(invalid); }, "guest interrupt delivery");
        Require(invalid.externalInterrupt->delivered == before.delivered &&
                    invalid.externalInterrupt->suspended.empty() && invalid.externalInterruptsEnabled,
                "Unsupported vector/system-call nesting changed exception state.");
    }
    for (unsigned limit : {2u, 4u, 7u}) {
        auto state = create();
        state.gpr[29] = 4;
        Reject([&] { dispatcher.Execute(state, 0x80001000, limit); }, "budget");
        Require(!state.callException && !state.callGuest && state.externalInterrupt->returned == 0,
                "Nested interrupt bypassed block limits or leaked callbacks.");
    }
    for (unsigned depth : {1u, 2u}) {
        auto state = create();
        state.gpr[29] = 4;
        Reject([&] { dispatcher.Execute(state, 0x80001000, 100, depth); }, "call-depth limit");
        Require(!state.callException && !state.callGuest && state.externalInterrupt->returned == 0,
                "Nested interrupt bypassed call-depth limits or leaked callbacks.");
    }
}

void TestDecrementerDelivery() {
    uint64_t ticks = 100;
    const auto create = [&] {
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.memory.resize(0x4000);
        state.externalInterrupt.emplace();
        state.decrementer.emplace();
        state.serialInterface.emplace();
        state.piInterruptMask = 8;
        state.readTimeBase = [&] { return ticks; };
        state.msr = 0x1032;
        state.externalInterruptsEnabled = true;
        state.lr = 0xfffffffcu;
        for (uint32_t address : {0x500u, 0x900u, 0x1000u, 0x2000u, 0x2100u, 0x2200u}) {
            ppc_write_u32(state, address, 0x60000000u);
        }
        return state;
    };
    auto initial = create();
    NativeDispatcher dispatcher;
    dispatcher.Register(0, initial, {
        {0x900u, [](PpcState& state) {
            Require(state.externalInterrupt->vector == 0x900u &&
                        state.srr0 == 0x80001000u && state.srr1 == 0x9032u &&
                        !state.externalInterruptsEnabled && state.lr == 0xfffffffcu,
                    "DEC exception did not preserve PC, ME/MSR and LR.");
            state.srr0 = 0x80002000u; state.srr1 = 0x30;
            ppc_0000B5C0(state);
        }, {0x60000000u}},
        {0x80002000u, [](PpcState& state) {
            ++state.gpr[28];
            if (state.gpr[29]) {
                state.serialInterface->communication = 0xc0000000u;
                ppc_write_msr(state, 0x9032);
            }
            state.pc = 0x80002100u;
        }, {0x60000000u}},
        {0x500u, [](PpcState& state) {
            Require(state.externalInterrupt->vector == 0x500u &&
                        state.externalInterrupt->suspended.size() == 1 &&
                        state.externalInterrupt->suspended.back().vector == 0x900u,
                    "External IRQ did not retain its suspended decrementer parent.");
            state.srr0 = 0x80002200u; state.srr1 = 0x30;
            ppc_0000B5C0(state);
        }, {0x60000000u}},
        {0x80002200u, [](PpcState& state) {
            ppc_write_u32(state, 0xcc006434u, 0xc0000000u);
            state.srr0 = state.externalInterrupt->resume;
            state.srr1 = state.externalInterrupt->savedMsr;
            ppc_0000B5C0(state);
        }, {0x60000000u}},
        {0x80002100u, [](PpcState& state) {
            Require(state.externalInterrupt->vector == 0x900u &&
                        state.externalInterrupt->suspended.size() == state.gpr[27],
                    "Nested IRQ lost the active decrementer vector.");
            ppc_write_msr(state, 0x1032);
            state.srr0 = state.externalInterrupt->resume;
            state.srr1 = state.externalInterrupt->savedMsr;
            ppc_0000B5C0(state);
        }, {0x60000000u}},
        {0x80001000u, [](PpcState& state) { ++state.gpr[31]; state.pc = state.lr; },
         {0x60000000u}}
    });
    for (bool nested : {false, true}) {
        ticks = 100;
        auto state = create();
        state.gpr[29] = nested;
        ppc_write_decrementer(state, 0);
        dispatcher.Execute(state, 0x80001000u);
        Require(state.decrementer->delivered == 0 && state.gpr[31] == 1,
                "DEC zero triggered before its nonnegative-to-negative transition.");
        state.externalInterruptsEnabled = false;
        ticks = 101;
        dispatcher.Execute(state, 0x80001000u);
        Require(state.decrementer->delivered == 0 && state.decrementer->deadline == 101,
                "Masked decrementer expiry was discarded or delivered.");
        Reject([&] { ppc_write_msr(state, 0x9032); }, "timer exception delivery");
        state.externalInterruptsEnabled = true;
        dispatcher.Execute(state, 0x80001000u);
        Require(state.decrementer->delivered == 1 && state.decrementer->returned == 1 &&
                    state.decrementer->deadline == 101 + (uint64_t{1} << 32) &&
                    state.externalInterrupt->delivered == unsigned(nested) &&
                    state.externalInterrupt->returned == unsigned(nested) &&
                    !state.externalInterrupt->active && state.externalInterrupt->suspended.empty() &&
                    state.gpr[28] == 1 && state.gpr[31] == 3 &&
                    ppc_read_msr(state) == 0x9032 && !state.callGuest && !state.callException,
                "DEC delivery, periodic wrap, nested IRQ or original continuation differs.");
        ticks += 3 * (uint64_t{1} << 32);
        dispatcher.Execute(state, 0x80001000u);
        Require(state.decrementer->delivered == 2 && state.decrementer->returned == 2 &&
                    state.decrementer->deadline > ticks && state.gpr[28] == 2,
                "Late DEC wraps were not coalesced into one pending exception.");
    }
    for (unsigned budget : {1u, 2u}) {
        ticks = 100;
        auto state = create();
        ppc_write_decrementer(state, 0);
        ticks = 101;
        Reject([&] { dispatcher.Execute(state, 0x80001000u, budget); }, "budget");
        Require(state.decrementer->delivered == 1 && state.decrementer->returned == 0 &&
                    !state.callGuest && !state.callException,
                "DEC delivery bypassed budgets or fabricated context restoration.");
    }
    ticks = 100;
    auto nestedTimer = create();
    nestedTimer.gpr[27] = 1;
    nestedTimer.externalInterrupt->active = true;
    nestedTimer.externalInterrupt->inHandler = true;
    nestedTimer.externalInterrupt->resume = 0x80003000u;
    nestedTimer.externalInterrupt->savedMsr = 0x8032u;
    ppc_write_decrementer(nestedTimer, 0);
    ticks = 101;
    dispatcher.Execute(nestedTimer, 0x80001000u);
    Require(nestedTimer.decrementer->delivered == 1 && nestedTimer.decrementer->returned == 1 &&
                nestedTimer.externalInterrupt->active && nestedTimer.externalInterrupt->inHandler &&
                nestedTimer.externalInterrupt->vector == 0x500u &&
                nestedTimer.externalInterrupt->resume == 0x80003000u &&
                nestedTimer.externalInterrupt->savedMsr == 0x8032u &&
                nestedTimer.externalInterrupt->suspended.empty(),
            "A decrementer nested inside an external handler lost its suspended parent.");
    ticks = 100;
    auto shallow = create();
    ppc_write_decrementer(shallow, 0);
    ticks = 101;
    Reject([&] { dispatcher.Execute(shallow, 0x80001000u, 100, 1); }, "call-depth");
    Require(!shallow.callGuest && !shallow.callException && shallow.decrementer->returned == 0,
            "DEC delivery bypassed call-depth limits or leaked callbacks.");
    ticks = 100;
    auto changed = create();
    ppc_write_decrementer(changed, 0);
    ticks = 101;
    ppc_write_u32(changed, 0x900u, 0);
    Reject([&] { dispatcher.Execute(changed, 0x80001000u); }, "code mismatch");
    Require(changed.decrementer->returned == 0 && !changed.callGuest && !changed.callException,
            "DEC vector integrity failure fabricated a return or leaked dispatcher ownership.");
}

void TestCompiledSystemCall() {
    for (uint32_t mode : {0u, 0x30u, 0x32u, 0x2032u, 0x9032u, 0xa032u, 0xb032u}) {
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.memory.resize(0x10000);
        state.msr = mode & ~0x8000u;
        state.externalInterruptsEnabled = (mode & 0x8000u) != 0;
        state.lr = 0xf000u;
        state.cr = 0x12345678u;
        state.xer = 0xe0000000u;
        ppc_write_u32(state, 0xb5b0u, 0x44000002u);
        ppc_write_u32(state, 0xb5b4u, 0x4e800020u);
        ppc_write_u32(state, 0x2000u, 0x4c000064u);
        NativeDispatcher dispatcher;
        dispatcher.Register(0, state, {
            {0xb5b0u, ppc_0000B5B0, {0x44000002u}},
            {0xb5b4u, [](PpcState& guest) { guest.pc = guest.lr; }, {0x4e800020u}},
            {0xc00u, [](PpcState& guest) {
                 Require(guest.systemCallActive && ppc_read_msr(guest) == 0 &&
                             guest.lr == 0xf000u && guest.srr0 == 0xb5b4u,
                         "System-call entry changed LR or failed exception MSR/SRR capture.");
                 ppc_0000B5C0(guest);
             }, {0x4c000064u}, 0x2000u}});
        Reject([&] { dispatcher.Execute(state, 0xb5b0u); }, "code mismatch");
        Require(state.systemCallActive && state.srr1 == mode &&
                    !state.callGuest && !state.callException && state.pc == 0xc00u,
                "Uninstalled exception vector bypassed integrity checks or leaked callbacks.");
        state.systemCallActive = false;
        state.msr = mode & ~0x8000u;
        state.externalInterruptsEnabled = (mode & 0x8000u) != 0;
        ppc_write_u32(state, 0xc00u, 0x4c000064u);
        dispatcher.Execute(state, 0xb5b0u);
        Require(!state.systemCallActive && ppc_read_msr(state) == mode && state.srr1 == mode &&
                    state.pc == 0xf000u && state.lr == 0xf000u &&
                    state.cr == 0x12345678u && state.xer == 0xe0000000u &&
                    !state.callGuest && !state.callException,
                "Compiled sc/rfi did not restore MSR, EE, continuation, LR or callback ownership.");
        state.externalInterrupt.emplace();
        state.externalInterrupt->active = true;
        state.externalInterrupt->inHandler = true;
        state.externalInterrupt->resume = 0x9000;
        state.externalInterrupt->savedMsr = 0x8032;
        state.externalInterrupt->delivered = 3;
        state.externalInterrupt->returned = 2;
        dispatcher.Execute(state, 0xb5b0u);
        Require(!state.systemCallActive && state.externalInterrupt->active &&
                    state.externalInterrupt->inHandler && state.externalInterrupt->resume == 0x9000 &&
                    state.externalInterrupt->savedMsr == 0x8032 && state.externalInterrupt->delivered == 3 &&
                    state.externalInterrupt->returned == 2 && state.pc == 0xf000u &&
                    ppc_read_msr(state) == mode && state.lr == 0xf000u,
                "System call inside a guest interrupt handler prematurely returned the outer exception.");
        state.externalInterrupt->inHandler = false;
        Reject([&] { dispatcher.Execute(state, 0xb5b0u); }, "system-call context");
        state.externalInterrupt.reset();
        Reject([&] { dispatcher.Execute(state, 0xb5b0u, 2); }, "budget exceeded");
        Require(!state.callException && !state.callGuest && !state.systemCallActive,
                "Exception dispatch did not share the block budget or restore its callbacks.");
        Reject([&] { dispatcher.Execute(state, 0xb5b0u, 100, 1); }, "call-depth limit exceeded");
        Require(state.systemCallActive && !state.callException && !state.callGuest,
                "Exception dispatch bypassed the depth bound or leaked callbacks.");
        state.systemCallActive = false;
        state.msr = mode & ~0x8000u;
        state.externalInterruptsEnabled = (mode & 0x8000u) != 0;
        state.callException = [&](uint32_t target, uint32_t resume, PpcState& guest) {
            Require(target == 0xc00u && resume == 0xb5d4u && guest.lr == 0xf000u,
                    "Default compiled sc changed its exception target/continuation or LR.");
            ppc_0000B5C0(guest);
        };
        state.gpr[3] = 7;
        ppc_0000B5D0(state);
        Require(state.gpr[3] == 8 && state.pc == 0xb5d8u && ppc_read_msr(state) == mode,
                "Default function translation did not continue after a system call.");
    }
    PpcState state;
    Reject([&] { ppc_0000B5B0(state); }, "system-call context");
    Reject([&] { ppc_0000B5C0(state); }, "active system-call");
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.callException = [](uint32_t, uint32_t, PpcState&) {};
    state.systemCallActive = true;
    Reject([&] { ppc_0000B5B0(state); }, "nested");
    state.systemCallActive = false;
    Reject([&] { ppc_0000B5B0(state); }, "without rfi");
    state.srr0 = 1;
    Reject([&] { ppc_0000B5C0(state); }, "supported active");
    state.srr0 = 0;
    state.srr1 = 0x40;
    Reject([&] { ppc_0000B5C0(state); }, "supported active");
    state.hid0 = 0xc200u;
    ppc_write_hid0(state, 0xc208u);
    Require(state.hid0 == 0xc208u, "HID0 data-cache flush assist did not retain its bit.");
    ppc_write_hid0(state, 0xc200u);
    Require(state.hid0 == 0xc200u, "HID0 data-cache flush assist did not restore its bit.");
    state.memory.resize(0x4000);
    ppc_write_u32(state, 0x2000u, 0x4c000064u);
    NativeDispatcher dispatcher;
    Reject([&] { dispatcher.Register(0, state,
        {{0xc00u, ppc_0000B5C0, {0x4c000064u}, 0x2002u}}); }, "installation source");
    Reject([&] { dispatcher.Register(0, state,
        {{0xc00u, ppc_0000B5C0, {0x4c000064u}, 0x2004u}}); }, "code mismatch");
    Require(!dispatcher.Contains(0xc00u), "Invalid vector source partially registered a binding.");
}

void TestDspResetAndAramDma() {
    for (bool read : {false, true}) {
        for (unsigned length : {32u, 64u, 128u}) {
            PpcState state;
            state.memoryMap = PpcMemoryMap::GameCubeRam;
            state.memory.resize(256, 0xa5);
            state.dspControl = 4;
            state.dspHardware.emplace();
            auto& dsp = *state.dspHardware;
            dsp.aram.resize(256, 0x3c);
            ppc_write_u32(state, 0xcc005020u, 64);
            ppc_write_u32(state, 0xcc005024u, 32);
            ppc_write_u32(state, 0xcc005028u, (read ? 0x80000000u : 0u) | length);
            Require(state.dspControl == 0x24u && dsp.dmaTransfers == 1 &&
                        ppc_read_u32(state, 0xcc005020u) == 64 + length &&
                        ppc_read_u32(state, 0xcc005024u) == 32 + length &&
                        ppc_read_u32(state, 0xcc005028u) == (read ? 0x80000000u : 0u),
                    "ARAM DMA completion, direction, counter or register readback differs.");
            for (unsigned index = 0; index < state.memory.size(); ++index) {
                Require(state.memory[index] == (read && index >= 64 && index < 64 + length ? 0x3cu : 0xa5u) &&
                            dsp.aram[index] == (!read && index >= 32 && index < 32 + length ? 0xa5u : 0x3cu),
                        "ARAM DMA copied the wrong bytes or changed RAM/ARAM guards.");
            }
            ppc_write_u16(state, 0xcc00500au, 0x24);
            Require(state.dspControl == 4, "ARAM interrupt W1C did not acknowledge DMA.");
        }
    }
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(256, 0xa5);
    state.dspHardware.emplace();
    auto& dsp = *state.dspHardware;
    dsp.aram.resize(256, 0x3c);
    state.dspControl = 0xac;
    dsp.toDspMailbox = 0xffffffffu;
    dsp.fromDspMailbox = 0xffffffffu;
    Reject([&] { ppc_read_u16(state, 0xcc005006u); }, "mailbox consumption");
    ppc_write_u16(state, 0xcc005012u, 0x43);
    Require(ppc_read_u16(state, 0xcc005012u) == 0x43, "ARAM configuration readback differs.");
    ppc_write_u16(state, 0xcc00500au, 0x8ac);
    ppc_write_u16(state, 0xcc00500au, 0x805);
    Require(state.dspControl == 0x804 && dsp.resetCount == 1 &&
                dsp.toDspMailbox == 0 && dsp.fromDspMailbox == 0 &&
                ppc_read_u16(state, 0xcc005004u) == 0 && ppc_read_u16(state, 0xcc005006u) == 0 &&
                ppc_read_u16(state, 0xcc005000u) == 0 && ppc_read_u16(state, 0xcc005002u) == 0 &&
                dsp.aram.front() == 0x3c,
            "DSP halted reset did not clear mailboxes/status or changed ARAM.");
    ppc_write_u16(state, 0xcc00500au, 4);
    Reject([&] { ppc_write_u16(state, 0xcc00500au, 0); }, "DSP unhalt");
    Require(state.dspControl == 4 && dsp.resetCount == 1 && dsp.fromDspMailbox == 0,
            "Unsupported DSP execution fabricated mailbox completion or cleared halt.");
    for (auto value : {0u, 1u, 31u, 33u, 0x04000020u, UINT32_MAX}) {
        const auto before = state.memory;
        const auto aram = dsp.aram;
        Reject([&] { ppc_write_u32(state, 0xcc005028u, value); }, "transfer length");
        Require(state.memory == before && dsp.aram == aram && state.dspControl == 4,
                "Invalid ARAM DMA partially committed memory or completion.");
    }
    ppc_write_u32(state, 0xcc005020u, 256);
    Reject([&] { ppc_write_u32(state, 0xcc005028u, 32); }, "outside guest memory");
    ppc_write_u32(state, 0xcc005020u, 0);
    ppc_write_u32(state, 0xcc005024u, 256);
    Reject([&] { ppc_write_u32(state, 0xcc005028u, 32); }, "configured native ARAM");
    for (auto address : {0xcc005020u, 0xcc005024u}) {
        Reject([&] { ppc_write_u32(state, address, 1); }, "aligned");
    }
    Reject([&] { ppc_write_u16(state, 0xcc005012u, 0); }, "configuration");
    Reject([&] { ppc_write_u16(state, 0xcc005002u, 1); }, "execution backend");
    Reject([&] { ppc_write_u16(state, 0xcc005000u, 1); }, "execution backend");
    Reject([&] { ppc_write_u16(state, 0xcc00500au, 0xffff); }, "control write");
    state.piInterruptMask = 0xf0u;
    state.externalInterruptsEnabled = false;
    state.dspControl = 0x64;
    Reject([&] { ppc_write_msr(state, 0x8000u); }, "DSP interrupt");
    Require(!state.externalInterruptsEnabled, "Pending DSP interrupt was silently discarded when enabling EE.");
    ppc_write_u16(state, 0xcc00500au, 0x64);
    ppc_write_msr(state, 0x8000u);
    Require(state.externalInterruptsEnabled && state.dspControl == 0x44,
            "DSP interrupt acknowledgement failed to retain HALT/mask.");
}

void TestMultiplyHighUnsigned() {
    for (uint32_t left : {0u, 1u, 0x7fffffffu, 0x80000000u, UINT32_MAX}) {
        for (uint32_t right : {0u, 1u, 2u, 0x80000000u, UINT32_MAX}) {
            for (bool record : {false, true}) {
                PpcState state;
                state.gpr[4] = left;
                state.gpr[5] = right;
                state.cr = 0x12345678u;
                state.xer = 0xe0000000u;
                if (record) ppc_0000B510(state);
                else ppc_0000B500(state);
                const uint32_t expected = static_cast<uint32_t>(
                    (static_cast<uint64_t>(left) * right) >> 32);
                const uint32_t field = expected == 0 ? 3u :
                    (expected & 0x80000000u) != 0 ? 9u : 5u;
                Require(state.gpr[3] == expected && state.gpr[4] == left &&
                            state.gpr[5] == right && state.xer == 0xe0000000u &&
                            state.cr == (record ? (field << 28) | 0x02345678u : 0x12345678u),
                        "Unsigned high multiply result, record flags or input preservation differs.");
            }
        }
    }
}

void TestMultiplyHighSigned() {
    using Function = void (*)(PpcState&);
    const Function functions[] = {ppc_0000B7E0, ppc_0000B7F0, ppc_0000B800, ppc_0000B810};
    for (uint32_t left : {0u, 1u, 0x55555556u, 0x7fffffffu, 0x80000000u, UINT32_MAX}) {
        for (uint32_t right : {0u, 1u, 3u, 0x7fffffffu, 0x80000000u, UINT32_MAX}) {
            for (unsigned operation = 0; operation < 4; ++operation) {
                PpcState state;
                state.gpr[4] = left;
                state.gpr[5] = right;
                state.cr = 0x12345678;
                state.xer = 0xe0000000;
                const auto product = int64_t(static_cast<int32_t>(left)) * static_cast<int32_t>(right);
                const auto expected = static_cast<uint32_t>(static_cast<uint64_t>(product) >> 32);
                functions[operation](state);
                const unsigned destination = operation < 2 ? 3 : operation == 2 ? 4 : 5;
                const uint32_t field = expected == 0 ? 3u : expected & 0x80000000u ? 9u : 5u;
                Require(state.gpr[destination] == expected && state.xer == 0xe0000000 &&
                            state.cr == (operation & 1 ? (field << 28) | 0x02345678u : 0x12345678u) &&
                            (destination == 4 || state.gpr[4] == left) &&
                            (destination == 5 || state.gpr[5] == right),
                        "Signed high multiply lost sign, aliased input or CR/XER state.");
            }
        }
    }
}

void TestNativeConsoleIntegers() {
    PpcState state;
    state.memory.resize(0x8000);
    const auto put = [&](uint32_t address, const std::string& text) {
        for (size_t index = 0; index <= text.size(); ++index) {
            ppc_write_u8(state, address + static_cast<uint32_t>(index),
                         index == text.size() ? 0 : static_cast<uint8_t>(text[index]));
        }
    };
    ppc_write_u8(state, 0x400, 7);
    ppc_write_u32(state, 0x404, 0x600);
    ppc_write_u32(state, 0x408, 0x500);
    state.gpr[4] = 0x400;
    state.lr = 0x888;
    std::string output;
    state.writeConsole = [&](const std::string& text) { output += text; };
    struct Case { const char* format; uint32_t value; const char* expected; };
    for (const auto& test : std::vector<Case>{
            {"%d", 0, "0"}, {"%d", 0x7fffffffu, "2147483647"},
            {"%i", 0x80000000u, "-2147483648"}, {"%d", UINT32_MAX, "-1"},
            {"%u", UINT32_MAX, "4294967295"}, {"%08x", 0xabu, "000000ab"},
            {"%X", UINT32_MAX, "FFFFFFFF"}, {"%12d", 0x80000000u, " -2147483648"},
            {"%012d", 0x80000000u, "-02147483648"}, {"%2u", UINT32_MAX, "4294967295"}}) {
        put(0x100, test.format);
        ppc_write_u32(state, 0x51c, test.value);
        state.gpr[3] = 0x100;
        output.clear();
        const auto memory = state.memory;
        NativeGuestVprintf(state);
        Require(output == test.expected && state.gpr[3] == output.size() &&
                    state.pc == state.lr && state.memory == memory,
                "Native 32-bit integer formatting, sign, padding, ABI or RAM preservation differs.");
    }
    put(0x100, "%d/%08X");
    ppc_write_u32(state, 0x51c, UINT32_MAX);
    ppc_write_u32(state, 0x600, 0x1234u);
    state.gpr[3] = 0x100;
    output.clear();
    NativeGuestVprintf(state);
    Require(output == "-1/00001234", "Integer arguments failed at the GPR-to-stack boundary.");
    for (const std::string format : {"prefix%d%f", "%n", "%4097u", "%999999999999999u",
                                     "%08", "%#x", "%lu", "%0s"}) {
        put(0x100, format);
        state.gpr[3] = 0x100;
        output.clear();
        Reject([&] { NativeGuestVprintf(state); }, "vprintf");
        Require(output.empty() && state.gpr[3] == 0x100,
                "Rejected console format emitted partial text or a success-shaped return.");
    }
    put(0x2000, std::string(4095, 'x'));
    ppc_write_u8(state, 0x400, 8);
    for (unsigned index = 0; index < 16; ++index) ppc_write_u32(state, 0x600 + 4 * index, 0x2000);
    std::string format;
    for (unsigned index = 0; index < 16; ++index) format += "%s";
    format += "abcdefghijklmnop";
    put(0x100, format);
    state.gpr[3] = 0x100;
    output.clear();
    NativeGuestVprintf(state);
    Require(output.size() == 65536 && state.gpr[3] == 65536,
            "Native console rejected the exact 64 KiB output limit.");
    put(0x100, format + "%%");
    state.gpr[3] = 0x100;
    output.clear();
    Reject([&] { NativeGuestVprintf(state); }, "64 KiB");
    Require(output.empty() && state.gpr[3] == 0x100,
            "Trailing percent bypassed the exact 64 KiB output bound.");
}

void TestRealModeRamConfiguration() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize);
    state.externalInterruptsEnabled = false;
    state.msr = 0x32u;
    state.ramBats.emplace();
    state.gpr[3] = 0x1000u;
    state.gpr[4] = 2;
    ppc_0000B5E0(state);
    Require(state.srr0 == 0x1000u && state.srr1 == 2, "Compiled SRR transfers lost their source.");
    ppc_0000B5C0(state);
    Require(state.pc == 0x1000u && state.msr == 2 && state.ramBats->realModeActive &&
                state.ramBats->transitions == 1,
            "Compiled real-mode rfi did not disable IR/DR or retain its physical PC.");
    const PpcRamBatState profile;
    for (unsigned index : {0u, 4u, 8u, 12u}) {
        ppc_write_ram_bat(state, 528 + index, 0);
        ppc_write_ram_bat(state, 529 + index, profile.values[index + 1]);
        state.srr0 = 0x80002000u;
        state.srr1 = 0x32u;
        Reject([&] { ppc_0000B5C0(state); }, "exact 24 MiB");
        ppc_write_ram_bat(state, 528 + index, profile.values[index]);
    }
    state.gpr[3] = profile.values[0];
    ppc_0000B5F0(state);
    Require(state.ramBats->values == profile.values && state.ramBats->writes == 13,
            "Native BAT configuration ignored lower/upper words or a compiled transfer.");
    ppc_0000B5C0(state);
    Require(state.pc == 0x80002000u && state.msr == 0x32u &&
                !state.ramBats->realModeActive && state.ramBats->transitions == 2,
            "Compiled configured rfi did not restore IR/DR at the saved native PC.");
    Reject([&] { ppc_write_ram_bat(state, 528, 0); }, "real-mode context");
    state.ramBats->realModeActive = true;
    state.msr = 2;
    for (auto pair : {std::make_pair(527u, 0u), std::make_pair(530u, 0u),
                      std::make_pair(544u, 0u), std::make_pair(528u, 0x800003ffu),
                      std::make_pair(529u, 3u)}) {
        Reject([&] { ppc_write_ram_bat(state, pair.first, pair.second); }, "BAT write");
    }
    state.srr0 = 0xc0002000u;
    Reject([&] { ppc_0000B5C0(state); }, "exact 24 MiB");
    state.srr0 = 0x80002000u;
    state.srr1 = 0x8032u;
    Reject([&] { ppc_0000B5C0(state); }, "real-mode rfi context");
    state.memoryProtection.emplace();
    state.miInterruptMask = 0;
    ppc_write_u16(state, 0xcc004010u, 0xff);
    ppc_write_u16(state, 0xcc004020u, 0);
    ppc_write_u16(state, 0xcc00401cu, 0x10);
    Require(state.memoryProtection->permissions == 0xff &&
                state.memoryProtection->acknowledgements == 1 && state.miInterruptMask == 0x10,
            "Native all-readable/writable memory-protection initialization differs.");
    Reject([&] { ppc_write_u16(state, 0xcc004010u, 0xfe); }, "read/write");
    Reject([&] { ppc_write_u16(state, 0xcc004020u, 1); }, "acknowledgement");
    Reject([&] { ppc_write_u16(state, 0xcc00401cu, 1); }, "MI interrupt mask");
    state.piResetCode = 0;
    Require(ppc_read_u32(state, 0xcc003024u) == 0, "Explicit cold-reset code differs.");
    state.piResetCode.reset();
    Reject([&] { ppc_read_u32(state, 0xcc003024u); }, "not mapped");
}

void TestNativeConsoleAndEmptyExi() {
    PpcState state;
    state.memory.resize(0x3000);
    const auto put = [&](uint32_t address, const std::string& text) {
        for (size_t i = 0; i <= text.size(); ++i) {
            ppc_write_u8(state, address + static_cast<uint32_t>(i),
                         i == text.size() ? 0 : static_cast<uint8_t>(text[i]));
        }
    };
    put(0x100, "[%s]%%:%s");
    put(0x200, "first");
    put(0x300, "second");
    state.gpr[3] = 0x100;
    state.gpr[4] = 0x400;
    state.lr = 0x888;
    ppc_write_u8(state, 0x400, 7);
    ppc_write_u32(state, 0x404, 0x600);
    ppc_write_u32(state, 0x408, 0x500);
    ppc_write_u32(state, 0x51c, 0x200);
    ppc_write_u32(state, 0x600, 0x300);
    unsigned calls = 0;
    state.writeConsole = [&](const std::string& text) {
        Require(++calls == 1 && text == "[first]%:second",
                "Native console did not format guest GPR/overflow strings.");
    };
    const auto memory = state.memory;
    NativeGuestVprintf(state);
    Require(calls == 1 && state.gpr[3] == 15 && state.pc == state.lr &&
                state.memory == memory,
            "Native console return count, return address or guest memory differs.");
    const auto reject = [&] {
        const uint32_t result = state.gpr[3];
        const uint32_t pc = state.pc;
        bool failed = false;
        try {
            NativeGuestVprintf(state);
        } catch (const std::runtime_error&) {
            failed = true;
        }
        Require(failed && state.gpr[3] == result && state.pc == pc && calls == 1,
                "Failed console output reported success or emitted partial text.");
    };
    put(0x100, "prefix%s%f");
    state.gpr[3] = 0x100;
    reject();
    put(0x100, "trailing%");
    reject();
    state.writeConsole = {};
    reject();
    state.writeConsole = [](const std::string&) { throw std::runtime_error("console failed"); };
    put(0x100, "literal");
    reject();
    std::fill(state.memory.begin() + 0x1000, state.memory.end(), 'x');
    state.gpr[3] = 0x1000;
    reject();
    for (unsigned length = 1; length <= 4; ++length) {
        for (unsigned direction = 0; direction < 3; ++direction) {
            for (auto port : {std::make_pair(0u, 0u), std::make_pair(0u, 2u),
                              std::make_pair(1u, 0u)}) {
                Require(TransferEmptyExternalExi({port.first, port.second, length, direction,
                                                 UINT32_MAX}) == 0,
                        "Explicit empty EXI port returned a device identity.");
            }
        }
    }
    for (auto port : {std::make_pair(0u, 1u), std::make_pair(2u, 0u),
                      std::make_pair(3u, 0u), std::make_pair(0u, 3u)}) {
        bool failed = false;
        try {
            (void)TransferEmptyExternalExi({port.first, port.second, 4, 0, 0});
        } catch (const std::runtime_error&) {
            failed = true;
        }
        Require(failed, "Unimplemented internal EXI device returned fake success.");
    }
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
            rejected = std::string(error.what()).find("Unsupported PI interrupt mask") != std::string::npos;
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
        if ((flag & 0xca08u) != 0) continue;
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
        ppc_0000FB00(state);
        Require(state.cr == ((cr & ~0x20000000u) | (((cr >> 31) | (cr >> 29)) & 1u) << 29),
                "Original camera cror did not merge comparison LT/EQ into the correct CR bit.");
        state.cr = cr;
        ppc_0000FB10(state);
        Require(state.cr == ((cr & ~(1u << 24)) | ((((cr >> 24) | (cr >> 23)) & 1u) << 24)),
                "Aliased cror destination/left changed the wrong bit or lost its original source.");
        state.cr = cr;
        ppc_0000FB20(state);
        Require(state.cr == ((cr & ~(1u << 23)) | ((((cr >> 24) | (cr >> 23)) & 1u) << 23)),
                "Aliased cror destination/right changed the wrong bit or lost its original source.");
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
    for (uint32_t left : {0u, 1u, 7u, 0xfffffff9u, 0x80000000u, 0x7fffffffu}) {
        for (uint32_t right : {0u, 1u, 3u, 0xfffffffdu, UINT32_MAX, 0x80000000u}) {
            PpcState state;
            state.gpr[3] = 0x12345678;
            state.gpr[4] = left;
            state.gpr[5] = right;
            state.cr = 0x12345678;
            state.xer = 0xe0000000;
            if (right == 0 || (left == 0x80000000u && right == UINT32_MAX)) {
                Reject([&] { ppc_0000F300(state); }, "Undefined PowerPC divw");
                Reject([&] { ppc_0000F310(state); }, "Undefined PowerPC divw");
                Require(state.gpr[3] == 0x12345678 && state.gpr[4] == left && state.cr == 0x12345678 &&
                            state.xer == 0xe0000000,
                        "Undefined signed division partially mutated CPU state.");
                continue;
            }
            const int64_t a = left <= INT32_MAX ? int64_t(left) : int64_t(left) - 0x100000000LL;
            const int64_t b = right <= INT32_MAX ? int64_t(right) : int64_t(right) - 0x100000000LL;
            const auto expected = static_cast<uint32_t>(a / b);
            ppc_0000F300(state);
            const uint32_t condition = expected == 0 ? 3 : expected >= 0x80000000u ? 9 : 5;
            Require(state.gpr[3] == expected && state.gpr[4] == left && state.gpr[5] == right &&
                        state.cr == ((0x12345678u & 0x0fffffffu) | (condition << 28)) &&
                        state.xer == 0xe0000000,
                    "Signed division truncation, CR0 or preserved carry/overflow differs.");
            const auto cr = state.cr;
            ppc_0000F310(state);
            Require(state.gpr[4] == expected && state.cr == cr, "Non-record aliased signed division differs.");
        }
    }
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
        std::vector<uint32_t> visited;
        dispatcher.Execute(state, entry, 3, 2, [&](uint32_t pc) { visited.push_back(pc); });
        Require(state.gpr[3] == (entry == 0x4000 ? 17u : 16u) &&
                    state.pc == 0x4ff0 && state.lr == 0x4ff0 &&
                    visited == std::vector<uint32_t>{entry, 0x4020u, 0x4040u},
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
    state.lr = 0x4ff0;
    state.gpr[3] = 10;
    dispatcher.Execute(state, 0x4000, 3, 2, [&](uint32_t pc) {
        if (pc == 0x4000) {
            auto invalid = module;
            invalid.back().expectedWords[0] ^= 1u;
            Reject([&] { dispatcher.Register(1, state, invalid); }, "code mismatch");
            Require(!dispatcher.Contains(0x4020), "Failed boundary registration leaked an entry.");
            Reject([&] { dispatcher.Unregister(0); }, "during execution");
            dispatcher.Register(1, state, module);
        }
    });
    Require(state.pc == 0x4ff0 && state.gpr[3] == 17,
            "Atomic native module registration at a block boundary failed.");
    state.lr = 0x4ff0;
    Reject([&] {
        dispatcher.Execute(state, 0x4040, 1, 1, [&](uint32_t) {
            ppc_write_u32(state, 0x4040, module.back().expectedWords[0] ^ 1u);
        });
    }, "code mismatch");
    ppc_write_u32(state, 0x4040, module.back().expectedWords[0]);
    state.lr = 0x4ff0;
    Reject([&] {
        dispatcher.Execute(state, 0x4040, 1, 1, [&](uint32_t) { state.pc = 0x4020; });
    }, "observer changed execution PC");
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
    nativeTestAction = [](PpcState&) {};
    dispatcher.Register(3, state, {{0x40c0, NativeTestHook, {0}}});
    Reject([&] {
        dispatcher.Execute(state, 0x4080, 100000, 256, [&](uint32_t pc) {
            if (pc == 0x4080) dispatcher.Unregister(2);
        });
    }, "active native module");
    Require(dispatcher.Contains(0x4080), "Block observer unregistered its active module.");
    nativeTestAction = [](PpcState& guest) { guest.pc = guest.lr; };
    dispatcher.Execute(state, 0x4080, 100000, 256, [&](uint32_t pc) {
        if (pc == 0x4080) dispatcher.Unregister(3);
    });
    Require(dispatcher.Contains(0x4080) && !dispatcher.Contains(0x40c0),
            "Block observer did not safely unregister an inactive module.");
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
    state.gpr[3] = 17;
    Reject([&] {
        dispatcher.Execute(state, 0x4040, 1, 1, [](uint32_t) {
            throw std::runtime_error("block observer failed");
        });
    }, "block observer failed");
    Require(!state.callGuest && !state.callException && state.gpr[3] == 17,
            "Throwing block observer executed code or leaked dispatcher callbacks.");
    dispatcher.Execute(state, 0x4040);
    Require(!state.callGuest, "Native dispatcher did not restore an empty callback.");
}

} // namespace

int main() {
    try {
        TestLogicalRegisters();
        TestRelocatedOperands();
        TestYieldingBranchesAndPrecisePc();
        TestFloatingPointTransfers();
        TestFloatingPointArithmetic();
        TestSingleTransfers();
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
        TestExiImmediateTransactions();
        TestMultiplyHighUnsigned();
        TestMultiplyHighSigned();
        TestSiStartupHardwareAndArithmetic();
        TestSiTransactionsAndPendingInterrupt();
        TestDiscDmaAndDeadline();
        TestNativeSramAndDma();
        TestCompiledSystemCall();
        TestCompiledInterruptRegisters();
        TestThreadContextReturn();
        TestExternalInterruptDelivery();
        TestNestedExternalInterruptDelivery();
        TestDecrementerDelivery();
        TestDspResetAndAramDma();
        TestCompiledDspInitialization();
        TestCompiledDspTask();
        TestCompiledIntegerFloatStore();
        TestNativeConsoleAndEmptyExi();
        TestRealModeRamConfiguration();
        TestNativeConsoleIntegers();
        TestAndComplement();
        TestCacheBlockInstructions();
        TestL2ControlRegisters();
        TestRegisterAddCarry();
        TestPairedLoads(false);
        TestPairedStores();
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
