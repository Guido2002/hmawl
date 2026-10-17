#include "ppc_state.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <random>
#include <stdexcept>
#include <utility>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename T, typename U>
T Bits(U value) {
    static_assert(sizeof(T) == sizeof(U));
    T result;
    std::memcpy(&result, &value, sizeof(result));
    return result;
}

uint64_t Execute(PpcState& state, uint64_t a, uint64_t b, PpcFloatOperation op,
                 bool single = true, uint32_t fpscr = 0) {
    state.fpr[1] = {a, 0x0123456789abcdef};
    state.fpr[2] = {b, 0xfedcba9876543210};
    state.fpr[3] = {0, 0x123456789abcdef0};
    state.fpscr = fpscr;
    PpcFloatArithmetic(state, 3, 1, 2, op, single);
    Require(state.fpr[1].ps0 == a && state.fpr[2].ps0 == b &&
                state.fpr[1].ps1 == UINT64_C(0x0123456789abcdef) &&
                state.fpr[2].ps1 == UINT64_C(0xfedcba9876543210) &&
                state.fpr[3].ps1 == (single ? state.fpr[3].ps0 : UINT64_C(0x123456789abcdef0)),
            "Floating arithmetic corrupted source or destination paired lanes.");
    return state.fpr[3].ps0;
}

void TestExactAndFlags() {
    PpcState state;
    using Op = PpcFloatOperation;
    Require(Execute(state, 0x4330000000000280, 0x4330000000000000, Op::Subtract) ==
                ppc_widen_single_bits(0x44200000) && state.fpscr == 0x4000,
            "Original GX integer-to-single subtraction did not produce exactly 640.");
    Require(Execute(state, 0x3ff0000000000000, 0x3ff0000000000000, Op::Add) ==
                0x4000000000000000 && state.fpscr == 0x4000,
            "Exact add result/FPRF differs.");
    Require(Execute(state, 0xbff0000000000000, 0x3fe0000000000000, Op::Multiply) ==
                0xbfe0000000000000 && state.fpscr == 0x8000,
            "Exact negative multiply result/FPRF differs.");
    Require(Execute(state, 0x7fefffffffffffff, 0x7fefffffffffffff, Op::Subtract) == 0 &&
                state.fpscr == 0x2000,
            "Cancellation of large double operands incorrectly overflowed single result.");
    for (unsigned mode = 0; mode < 4; ++mode) {
        Require(Execute(state, 0x3ff0000000000000, 0x3ff0000000000000,
                        Op::Subtract, false, mode) == (mode == 3 ? UINT64_C(0x8000000000000000) : 0),
                "Exact cancellation did not honor round-down negative zero.");
        Require(Execute(state, 0x8000000000000000, 0, Op::Multiply, false, mode) ==
                    UINT64_C(0x8000000000000000),
                "Multiplication lost signed zero.");
        const uint64_t expected = mode == 2 ? ppc_widen_single_bits(0x3f800001) : 0x3ff0000000000000;
        Require(Execute(state, 0x3ff0000000000000, 0x3e70000000000000,
                        Op::Add, true, mode) == expected &&
                    (state.fpscr & 0x82020000) == 0x82020000 &&
                    ((state.fpscr & 0x40000) != 0) == (mode == 2),
                "Halfway single rounding or FPSCR inexact/increment status differs.");
        const auto negative = Execute(state, 0xbff0000000000000, 0xbe70000000000000,
                                      Op::Add, true, mode);
        Require(negative == (mode == 3 ? ppc_widen_single_bits(0xbf800001) : UINT64_C(0xbff0000000000000)),
                "Negative halfway rounding mode differs.");
    }
    // A double-rounded add would incorrectly tie to 1.0f rather than the next float.
    Require(Execute(state, 0x3ff0000010000000, 0x3c90000000000000, Op::Add) ==
                ppc_widen_single_bits(0x3f800001),
            "Single add was incorrectly rounded through an intermediate double result.");
    Require(Execute(state, ppc_widen_single_bits(1), ppc_widen_single_bits(2), Op::Add) ==
                ppc_widen_single_bits(3) && state.fpscr == 0x14000,
            "Exact single subnormal arithmetic was flushed or classified as normal.");
    Require(Execute(state, ppc_widen_single_bits(1), 0x3fe0000000000000, Op::Multiply) == 0 &&
                state.fpscr == 0x8a022000,
            "Underflow/inexact sticky bits or zero classification differ.");
    Require(Execute(state, ppc_widen_single_bits(0x7f7fffff), 0x4000000000000000,
                    Op::Multiply) == 0x7ff0000000000000 && state.fpscr == 0x92065000,
            "Single overflow did not set OX/XX/FI/FR and positive infinity FPRF.");
    Require(Execute(state, ppc_widen_single_bits(0x7f7fffff), 0x4000000000000000,
                    Op::Multiply, true, 1) == ppc_widen_single_bits(0x7f7fffff),
            "Overflow toward zero did not return the largest finite single.");
    Require(Execute(state, 0x3ff0000000000000, 0x3e70000000000000,
                    Op::Add, true, 0x02000000) == 0x3ff0000000000000 &&
                (state.fpscr & 0x80000000) == 0,
            "Repeated sticky inexact incorrectly raised FX again.");
}

void TestSpecialAndRejectedModes() {
    PpcState state;
    using Op = PpcFloatOperation;
    Require(Execute(state, 0x3ff0000000000000, 0, Op::Divide) == 0x7ff0000000000000 &&
                state.fpscr == 0x84005000,
            "Finite division by zero did not raise ZX/FX.");
    Require(Execute(state, 0, 0, Op::Divide) == 0x7ff8000000000000 &&
                state.fpscr == 0xa0211000,
            "Zero divided by zero did not raise VXZDZ.");
    Require(Execute(state, 0x7ff0000000000000, 0xfff0000000000000, Op::Divide) ==
                0x7ff8000000000000 && state.fpscr == 0xa0411000,
            "Infinity divided by infinity did not raise VXIDI.");
    Require(Execute(state, 0x7ff0000000000000, 0xfff0000000000000, Op::Add) ==
                0x7ff8000000000000 && state.fpscr == 0xa0811000,
            "Infinity cancellation did not raise VXISI/VX/FX or default NaN.");
    Require(Execute(state, 0x7ff0000000000000, 0, Op::Multiply) ==
                0x7ff8000000000000 && state.fpscr == 0xa0111000,
            "Infinity-times-zero did not raise VXIMZ.");
    Require(Execute(state, 0x7ff0000000001234, 0x3ff0000000000000, Op::Add, false) ==
                0x7ff8000000001234 && state.fpscr == 0xa1011000,
            "Signaling NaN did not quiet its payload and raise VXSNAN.");
    Require(Execute(state, 0xfff8000001234567, 0x7ff0000000000001, Op::Add, false) ==
                0xfff8000001234567 && state.fpscr == 0xa1011000,
            "NaN operand priority/sign/payload or second-operand signaling differs.");
    Require(Execute(state, 0x3ff0000000000000, 0x3ff0000000000000, Op::Add, true, 4) ==
                0x4000000000000000 && state.fpscr == 0x4004,
            "Non-IEEE normal arithmetic did not retain the original startup NI mode.");
    for (unsigned bit = 3; bit < 8; ++bit) {
        state.fpscr = 1u << bit;
        const auto original = state.fpr;
        bool rejected = false;
        try {
            PpcFloatArithmetic(state, 3, 1, 2, Op::Add, true);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        Require(rejected && state.fpscr == (1u << bit) && state.fpr == original,
                "Unsupported NI/exception mode did not fail atomically.");
    }
    state.fpscr = 0;
    state.fpr[2].ps0 = 0x3ff0000000000001;
    bool rejected = false;
    state.fpr[1].ps0 = 0x3ff0000000000000;
    PpcFloatArithmetic(state, 3, 1, 2, Op::Multiply, true);
    Require(state.fpr[3].ps0 == 0x3ff0000000000000 && state.fpscr == 0x4000,
            "Non-single multiplier was not rounded to Gekko's 25-bit significand before multiplying.");
    for (const bool denormalInput : {false, true}) {
        state.fpscr = 4;
        state.fpr[1].ps0 = ppc_widen_single_bits(denormalInput ? 1 : 0x00800000);
        state.fpr[2].ps0 = 0x3fe0000000000000;
        const auto before = state.fpr;
        rejected = false;
        try {
            PpcFloatArithmetic(state, 3, 1, 2, Op::Multiply, true);
        } catch (const std::runtime_error& error) {
            rejected = std::string(error.what()).find("non-IEEE denormal") != std::string::npos;
        }
        Require(rejected && state.fpscr == 4 && state.fpr == before,
                "Unsupported NI denormal input/result did not fail before mutation.");
    }
}

void TestCompareAndConversion() {
    PpcState state;
    for (const bool ordered : {false, true}) {
        for (const auto pair : {std::pair<uint64_t, uint64_t>{0, 0x8000000000000000},
                               {0xbff0000000000000, 0x3ff0000000000000},
                               {0x7ff8000000000000, 0x3ff0000000000000},
                               {0x7ff0000000000001, 0x3ff0000000000000}}) {
            state.fpr[1].ps0 = pair.first;
            state.fpr[2].ps0 = pair.second;
            state.fpscr = 0x70004;
            state.cr = 0x12345678;
            const uint32_t condition = pair.first == 0 ? 2 :
                pair.first == UINT64_C(0xbff0000000000000) ? 8 : 1;
            const uint32_t flags = (pair.first == UINT64_C(0x7ff0000000000001) ? 0x01000000u : 0) |
                (condition == 1 && ordered ? 0x00080000u : 0);
            PpcFloatCompare(state, 2, 1, 2, ordered);
            Require(state.cr == ((0x12345678u & ~0x00f00000u) | (condition << 20)) &&
                        state.fpscr == (0x70004u | (condition << 12) | flags |
                            (flags ? 0xa0000000u : 0)),
                    "Floating comparison signed zero, NaN exception or preserved FPRF/FI/FR differs.");
        }
    }
    struct Conversion { uint64_t input; uint64_t result; uint32_t fpscr; };
    const Conversion cases[] = {
        {0, 0xfff8000000000000, 0},
        {0x8000000000000000, 0xfff8000100000000, 0},
        {0x3ff8000000000000, 0xfff8000000000001, 0x82020000},
        {0xbff8000000000000, 0xfff80000ffffffff, 0x82020000},
        {0xbfe0000000000000, 0xfff8000100000000, 0x82020000},
        {0x41dfffffffc00000, 0xfff800007fffffff, 0},
        {0xc1e0000000000000, 0xfff8000080000000, 0},
        {0x41e0000000000000, 0xfff800007fffffff, 0xa0000100},
        {0xc1e0000000200000, 0xfff8000080000000, 0xa0000100},
        {0x7ff8000000000001, 0xfff8000080000000, 0xa0000100},
        {0x7ff0000000000000, 0xfff800007fffffff, 0xa0000100},
        {0xfff0000000000000, 0xfff8000080000000, 0xa0000100},
    };
    for (const auto& test : cases) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            state.fpr[1] = {test.input, 0x0123456789abcdef};
            state.fpscr = mode | 0x15000;
            PpcFloatToInteger(state, 1, 1);
            Require(state.fpr[1] == PpcFpr{test.result, 0x0123456789abcdef} &&
                        state.fpscr == (test.fpscr | mode | 0x15000),
                    "Truncating integer conversion range, special value, alias, rounding independence or FPSCR differs.");
        }
    }
}

uint64_t Random(uint64_t& seed) {
    seed ^= seed << 13;
    seed ^= seed >> 7;
    seed ^= seed << 17;
    return seed;
}

void TestPairedArithmetic() {
    using Op = PpcFloatOperation;
    const uint32_t values[] = {0, 0x80000000, 1, 0x00800000, 0x3f800000, 0xbf800000,
                              0x33800000, 0x7f7fffff, 0x7f800000, 0xff800000,
                              0x7fc12345, 0x7f812345};
    for (const auto operation : {Op::Add, Op::Subtract, Op::Multiply, Op::Divide}) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            for (unsigned i = 0; i < std::size(values); ++i) {
                for (unsigned j = 0; j < std::size(values); ++j) {
                    PpcState paired, first, second;
                    paired.fpscr = first.fpscr = second.fpscr = mode;
                    paired.fpr[4] = {ppc_widen_single_bits(values[i]),
                                     ppc_widen_single_bits(values[j])};
                    paired.fpr[5] = {ppc_widen_single_bits(values[j]),
                                     ppc_widen_single_bits(values[(i + 3) % std::size(values)])};
                    first.fpr[4].ps0 = paired.fpr[4].ps0;
                    first.fpr[5].ps0 = paired.fpr[5].ps0;
                    second.fpr[4].ps0 = paired.fpr[4].ps1;
                    second.fpr[5].ps0 = paired.fpr[5].ps1;
                    PpcFloatArithmetic(first, 3, 4, 5, operation, true);
                    PpcFloatArithmetic(second, 3, 4, 5, operation, true);
                    PpcPairedArithmetic(paired, 4, 4, 5, operation);
                    constexpr uint32_t causes = 0x1ff80700u;
                    Require(paired.fpr[4] == PpcFpr{first.fpr[3].ps0, second.fpr[3].ps0} &&
                                (paired.fpscr & causes) == ((first.fpscr | second.fpscr) & causes) &&
                                (paired.fpscr & 0x1f000u) == (first.fpscr & 0x1f000u) &&
                                (paired.fpscr & 0x60000u) == ((first.fpscr | second.fpscr) & 0x60000u),
                            "Paired arithmetic differs from independent scalar lanes, sticky causes or lane-zero classification.");
                }
            }
        }
    }
    for (unsigned failure = 0; failure < 4; ++failure) {
        PpcState state;
        state.fpr[1] = {0x3ff0000000000000, 0x3ff0000000000000};
        state.fpr[2] = {0x4000000000000000, 0x3ff0000000000000};
        state.fpscr = failure == 0 ? 0x80 : failure == 1 ? 4 : 0;
        if (failure == 1) state.fpr[1].ps1 = ppc_widen_single_bits(1);
        if (failure == 2) {
            state.fpscr = 4;
            state.fpr[1].ps1 = ppc_widen_single_bits(0x00800000);
            state.fpr[2].ps1 = 0x3fe0000000000000;
        }
        const auto before = state.fpr;
        const auto status = state.fpscr;
        bool rejected = false;
        try {
            PpcPairedArithmetic(state, 1, 1, 2, Op::Multiply, failure == 3 ? 2 : -1);
        } catch (const std::runtime_error&) {
            rejected = true;
        }
        Require(rejected && state.fpr == before && state.fpscr == status,
                "Paired second-lane/unsupported-mode rejection partially changed FPRs or FPSCR.");
    }
}

void TestFusedAndSums() {
    PpcState state;
    // (1 + 2^-23) * (1 - 2^-23) - 1 = -2^-46, not zero.
    state.fpr[1] = {ppc_widen_single_bits(0x3f800001), 0x4000000000000000};
    state.fpr[2] = {ppc_widen_single_bits(0x3f7ffffe), 0x4008000000000000};
    state.fpr[3] = {0x3ff0000000000000, 0x4010000000000000};
    PpcFloatFused(state, 1, 1, 2, 3, true, true, true);
    Require(state.fpr[1] == PpcFpr{ppc_widen_single_bits(0xa8800000), 0x4000000000000000} &&
                state.fpscr == 0x8000,
            "Paired multiply-subtract rounded the product before cancellation or corrupted aliases.");
    state.fpr[2] = {0x3ff0000000000000, 0x4000000000000000};
    state.fpr[3] = {0x4008000000000000, 0x4010000000000000};
    state.fpr[4] = {0x4014000000000000, 0x4018000000000000};
    for (bool second : {false, true}) {
        auto sum = state;
        sum.fpr[4] = {0xc014000000000000, 0x7ff0000000001234};
        PpcPairedSum(sum, 4, 2, 3, 4, second);
        Require(sum.fpr[4] == (second ? PpcFpr{0xc014000000000000, 0x4014000000000000} :
                                             PpcFpr{0x4014000000000000, 0x7ff0000000001234}) &&
                    sum.fpscr == 0x4000,
                "Paired sum failed to copy raw lane bits or classified the copied lane instead of the sum.");
    }
    state.fpr[1].ps0 = UINT64_C(0x7fefffffffffffff);
    state.fpr[2].ps0 = UINT64_C(0x4000000000000000);
    state.fpr[3].ps0 = UINT64_C(0xffefffffffffffff);
    state.fpscr = 0;
    PpcFloatFused(state, 4, 1, 2, 3, false, false, false);
    Require(state.fpr[4].ps0 == UINT64_C(0x7fefffffffffffff) && state.fpscr == 0x4000,
            "Fused product overflowed before its cancelling addend.");
    for (unsigned mode = 0; mode < 4; ++mode) {
        state.fpr[1] = {0x3ff0000000000000, 0x3ff0000000000000};
        state.fpr[2] = {0x3ff0000000000000, 0x3ff0000000000000};
        state.fpr[3] = {0x3e70000000000000, 0x7ff8000020000000};
        state.fpscr = mode;
        PpcFloatFused(state, 4, 1, 2, 3, false, true, true, true);
        const auto rounded = mode == 2 ? ppc_widen_single_bits(0x3f800001) : UINT64_C(0x3ff0000000000000);
        Require(state.fpr[4] == PpcFpr{rounded ^ UINT64_C(0x8000000000000000), 0x7ff8000020000000} &&
                    (state.fpscr & 0x1f000u) == 0x8000,
                "Negative fused arithmetic negated before rounding or changed a propagated NaN sign.");
    }
    for (unsigned mode = 0; mode < 4; ++mode) {
        state.fpr[1] = {0x3ff0000000000000, 0x3ff0000000000000};
        state.fpr[2] = {0x3ff0000000000000, 0x3ff0000000000000};
        state.fpr[3] = {0x3e70000000000000, 0x3e70000000000000};
        state.fpscr = mode;
        PpcFloatFused(state, 4, 1, 2, 3, false, true, true);
        const auto expected = mode == 2 ? ppc_widen_single_bits(0x3f800001) : UINT64_C(0x3ff0000000000000);
        Require(state.fpr[4] == PpcFpr{expected, expected} &&
                    (state.fpscr & 0x82020003u) == (0x82020000u | mode) &&
                    bool(state.fpscr & 0x40000u) == (mode == 2),
                "Fused halfway rounding mode/inexact/increment status differs.");
        state.fpr[3] = state.fpr[1];
        state.fpscr = mode;
        PpcFloatFused(state, 4, 1, 2, 3, true, true, true);
        const uint64_t zero = mode == 3 ? UINT64_C(0x8000000000000000) : 0;
        Require(state.fpr[4] == PpcFpr{zero, zero},
                "Exact fused cancellation lost directed-rounding zero sign.");
    }
    for (unsigned special = 0; special < 4; ++special) {
        state.fpscr = 0;
        state.fpr[1] = {0x3ff0000000000000, 0x7ff0000000000000};
        state.fpr[2] = {0x4000000000000000, special == 0 ? 0 : UINT64_C(0x3ff0000000000000)};
        state.fpr[3] = {0x4008000000000000, 0xfff0000000000000};
        if (special == 2) state.fpr[1].ps1 = 0x7ff0000000001234;
        if (special == 3) state.fpr[1].ps1 = 0x7ff8000020000000;
        PpcFloatFused(state, 4, 1, 2, 3, false, true, true);
        const uint32_t flag = special == 0 ? 0x00100000u : special == 1 ? 0x00800000u :
                              special == 2 ? 0x01000000u : 0;
        Require(state.fpr[4].ps0 == 0x4014000000000000 &&
                    (state.fpr[4].ps1 & UINT64_C(0x7ff8000000000000)) == UINT64_C(0x7ff8000000000000) &&
                    (state.fpscr & 0x7f000u) == 0x4000 &&
                    (state.fpscr & 0x1ff80700u) == flag &&
                    bool(state.fpscr & 0x80000000u) == (flag != 0),
                "Fused special-value result/second-lane causes differ.");
    }
    uint64_t seed = 0x123456789abcdef0;
    for (unsigned i = 0; i < 20000; ++i) {
        const uint32_t a = uint32_t(Random(seed)), b = uint32_t(Random(seed)), c = uint32_t(Random(seed));
        if ((a & 0x7f800000) == 0x7f800000 || (b & 0x7f800000) == 0x7f800000 ||
            (c & 0x7f800000) == 0x7f800000) continue;
        state.fpr[1] = {ppc_widen_single_bits(a), ppc_widen_single_bits(c)};
        state.fpr[2] = {ppc_widen_single_bits(b), ppc_widen_single_bits(a)};
        state.fpr[3] = {ppc_widen_single_bits(c), ppc_widen_single_bits(b)};
        for (const bool subtract : {false, true}) {
            state.fpscr = 0;
            PpcFloatFused(state, 4, 1, 2, 3, subtract, true, true);
            const float x = Bits<float>(a), y = Bits<float>(b), z = Bits<float>(c);
            Require(state.fpr[4] == PpcFpr{
                        ppc_widen_single_bits(Bits<uint32_t>(std::fma(x, y, subtract ? -z : z))),
                        ppc_widen_single_bits(Bits<uint32_t>(std::fma(z, x, subtract ? -y : y)))},
                    "Exact fused paired arithmetic differs from independent finite single FMA reference.");
        }
        const auto da = Random(seed), db = Random(seed), dc = Random(seed);
        if (((da >> 52) & 2047) == 2047 || ((db >> 52) & 2047) == 2047 ||
            ((dc >> 52) & 2047) == 2047) continue;
        state.fpr[1].ps0 = da;
        state.fpr[2].ps0 = db;
        state.fpr[3].ps0 = dc;
        state.fpr[4].ps1 = 0x123456789abcdef0;
        state.fpscr = 0;
        PpcFloatFused(state, 4, 1, 2, 3, false, false, false);
        Require(state.fpr[4].ps0 ==
                    Bits<uint64_t>(std::fma(Bits<double>(da), Bits<double>(db), Bits<double>(dc))) &&
                    state.fpr[4].ps1 == 0x123456789abcdef0,
                "Portable exact fused accumulator differs from finite binary64 FMA or changed the scalar paired lane.");
    }
    for (unsigned failure = 0; failure < 3; ++failure) {
        state.fpscr = failure == 0 ? 0x80 : 4;
        state.fpr[1] = {0x3ff0000000000000, ppc_widen_single_bits(failure == 1 ? 1 : 0x00800000)};
        state.fpr[2] = {0x4000000000000000, 0x3fe0000000000000};
        state.fpr[3] = {0, 0};
        const auto before = state.fpr;
        const auto status = state.fpscr;
        bool rejected = false;
        try { PpcFloatFused(state, 1, 1, 2, 3, false, true, true); }
        catch (const std::runtime_error&) { rejected = true; }
        Require(rejected && state.fpr == before && state.fpscr == status,
                "Unsupported fused second-lane input/result/exception mode did not fail atomically.");
    }
}

void TestReciprocalSqrtEstimate() {
    PpcState state;
    const std::pair<uint64_t, uint64_t> known[] = {
        {0x3ff0000000000000, 0x3feffe8000000000},
        {0x4000000000000000, 0x3fe69fa000000000},
        {0x4010000000000000, 0x3fdffe8000000000},
        {0x3fe0000000000000, 0x3ff69fa000000000},
        {0, 0x7ff0000000000000}, {0x8000000000000000, 0xfff0000000000000},
        {0x7ff0000000000000, 0}, {0xfff0000000000000, 0x7ff8000000000000},
        {0xbff0000000000000, 0x7ff8000000000000},
        {0xfff8000023456789, 0xfff8000023456789}, {0x7ff0000000001234, 0x7ff8000000001234}
    };
    for (unsigned i = 0; i < std::size(known); ++i) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            state.fpscr = mode | 0x60000;
            state.fpr[1] = {known[i].first, 0x123456789abcdef0};
            PpcFloatReciprocalSqrtEstimate(state, 1, 1);
            const uint32_t flag = i == 4 || i == 5 ? 0x04000000u :
                                  i == 7 || i == 8 ? 0x200u : i == 10 ? 0x01000000u : 0;
            Require(state.fpr[1] == PpcFpr{known[i].second, 0x123456789abcdef0} &&
                        (state.fpscr & 0x1ff80703u) == (flag | mode) &&
                        (state.fpscr & 0x60000u) == 0,
                    "Gekko reciprocal square-root estimate anchor/special/alias or status differs.");
        }
    }
    for (unsigned index = 0; index < 65536; ++index) {
        for (uint64_t tail : {UINT64_C(0), (UINT64_C(1) << 37) - 1}) {
            const uint64_t input = (UINT64_C(1023) << 52) + (uint64_t(index) << 37) + tail;
            state.fpr[1].ps0 = input;
            state.fpscr = 0;
            PpcFloatReciprocalSqrtEstimate(state, 2, 1);
            const double estimate = Bits<double>(state.fpr[2].ps0);
            const double exact = 1.0 / std::sqrt(Bits<double>(input));
            Require(std::abs(estimate / exact - 1) < 1.0 / 4096,
                    "Gekko estimate ROM/interpolation exceeded its required relative-error bound.");
        }
    }
    for (const uint64_t input : {UINT64_C(1), UINT64_C(0x000fffffffffffff),
                                 UINT64_C(0x0010000000000000), UINT64_C(0x7fefffffffffffff)}) {
        state.fpr[1].ps0 = input;
        state.fpscr = 0;
        PpcFloatReciprocalSqrtEstimate(state, 2, 1);
        Require(std::abs(Bits<double>(state.fpr[2].ps0) * std::sqrt(Bits<double>(input)) - 1) < 1.0 / 4096,
                "Reciprocal square-root exponent normalization failed at a binary64 endpoint.");
    }
    for (const uint32_t mode : {4u, 8u, 16u, 32u, 64u, 128u}) {
        state.fpscr = mode;
        state.fpr[1].ps0 = 1;
        const auto original = state.fpr;
        bool rejected = false;
        try { PpcFloatReciprocalSqrtEstimate(state, 1, 1); }
        catch (const std::runtime_error&) { rejected = true; }
        Require(rejected && state.fpr == original && state.fpscr == mode,
                "Unsupported reciprocal-square-root NI/exception mode did not fail atomically.");
    }
}

void TestReciprocalEstimate() {
    PpcState state;
    const std::pair<uint64_t, uint64_t> known[] = {
        {0x3ff0000000000000, 0x3fefff0000000000},
        {0x4000000000000000, 0x3fdfff0000000000},
        {0xbff0000000000000, 0xbfefff0000000000},
        {0, 0x7ff0000000000000}, {0x8000000000000000, 0xfff0000000000000},
        {0x7ff0000000000000, 0}, {0xfff0000000000000, 0x8000000000000000},
        {0x7ff0000000001234, 0x7ff8000000001234},
        {0xfff8000023456789, 0xfff8000023456789},
        {1, 0x47efffffe0000000}, {0x8000000000000001, 0xc7efffffe0000000},
        {0x37efffffffffffff, 0x47efffffe0000000},
        {0x37f0000000000000, 0x47efff0000000000},
        {0x47d0000000000000, 0}, {0xc7d0000000000000, 0x8000000000000000}
    };
    for (unsigned index = 0; index < std::size(known); ++index) {
        for (unsigned mode = 0; mode < 4; ++mode) {
            state.fpr[1] = {known[index].first, 0x123456789abcdef0};
            state.fpscr = 0x60000 | mode;
            PpcFloatReciprocalEstimate(state, 1, 1);
            const auto flag = index == 3 || index == 4 ? 0x04000000u :
                              index == 7 ? 0x01000000u : 0;
            Require(state.fpr[1] == PpcFpr{known[index].second, known[index].second} &&
                        (state.fpscr & 0x1ff80703u) == (flag | mode) &&
                        (state.fpscr & 0x60000u) == (index < 3 || index >= 9 ? 0x60000u : 0),
                    "Reciprocal estimate anchor, saturation, aliases, flags or FR/FI differs.");
        }
    }
    for (unsigned index = 0; index < 32768; ++index) {
        for (const uint64_t tail : {UINT64_C(0), (UINT64_C(1) << 37) - 1}) {
            const auto input = UINT64_C(0x3ff0000000000000) | (uint64_t(index) << 37) | tail;
            state.fpr[1].ps0 = input;
            state.fpscr = 0;
            PpcFloatReciprocalEstimate(state, 2, 1);
            Require(std::abs(Bits<double>(state.fpr[2].ps0) * Bits<double>(input) - 1) < 1.0 / 4096,
                    "Reciprocal estimate exceeds the 1/4096 relative-error bound.");
        }
    }
    for (const uint32_t mode : {4u, 8u, 16u, 32u, 64u, 128u}) {
        state.fpscr = mode;
        state.fpr[1] = {1, 0x123456789abcdef0};
        const auto before = state.fpr;
        bool rejected = false;
        try { PpcFloatReciprocalEstimate(state, 1, 1); }
        catch (const std::runtime_error&) { rejected = true; }
        Require(rejected && state.fpr == before && state.fpscr == mode,
                "Unsupported reciprocal estimate mode did not fail atomically.");
    }
    for (bool second : {false, true}) {
        for (bool ordered : {false, true}) {
            for (const auto nan : {UINT64_C(0x7ff8000000000001), UINT64_C(0x7ff0000000000001)}) {
                state.fpr[1] = second ? PpcFpr{0, nan} : PpcFpr{nan, 0};
                state.fpr[2] = {0, 0};
                state.cr = 0x12345678;
                state.fpscr = 0;
                PpcFloatCompare(state, 7, 1, 2, ordered, second);
                Require(state.cr == 0x12345671 &&
                            (state.fpscr & 0x01080000u) ==
                                ((ordered ? 0x80000u : 0) |
                                 (nan == UINT64_C(0x7ff0000000000001) ? 0x01000000u : 0)),
                        "Paired ordered/unordered NaN comparison lost lane-specific exceptions.");
            }
        }
        state.fpr[1] = second ? PpcFpr{0x7ff0000000000001, 0} : PpcFpr{0, 0x7ff0000000000001};
        state.fpr[2] = {0x8000000000000000, 0x8000000000000000};
        state.fpscr = 0;
        PpcFloatCompare(state, 0, 1, 2, true, second);
        Require(state.fpscr == 0x2000, "Paired comparison inspected the unselected lane or distinguished signed zero.");
        for (const uint32_t mode : {4u, 8u, 16u, 32u, 64u, 128u}) {
            state.fpr[1] = {1, 1};
            state.fpscr = mode;
            state.cr = 0x12345678;
            const auto before = state.fpr;
            bool rejected = false;
            try { PpcFloatCompare(state, 0, 1, 2, true, second); }
            catch (const std::runtime_error&) { rejected = true; }
            Require(rejected && state.fpr == before && state.fpscr == mode && state.cr == 0x12345678,
                    "Unsupported paired comparison mode did not preserve CR/FPSCR and registers.");
        }
    }
}

void TestRoundSingleAndMultiplier() {
    PpcState state;
    for (unsigned mode = 0; mode < 4; ++mode) {
        for (uint64_t sign : {UINT64_C(0), UINT64_C(0x8000000000000000)}) {
            state.fpscr = mode;
            state.fpr[1] = {UINT64_C(0x3ff0000010000000) | sign, 0x123456789abcdef0};
            PpcFloatRoundSingle(state, 1, 1);
            const bool increment = sign ? mode == 3 : mode == 2;
            Require(state.fpr[1] == PpcFpr{(UINT64_C(0x3ff0000000000000) |
                                               (increment ? UINT64_C(0x20000000) : 0)) | sign,
                                           0x123456789abcdef0} &&
                        (state.fpscr & 0x60000) == (increment ? 0x60000 : 0x20000),
                    "frsp rounding mode/tie/sign/FI/FR or ps1 differs.");
            state.fpr[1].ps0 = 0x3ff0000000000000;
            state.fpr[2].ps0 = UINT64_C(0x3ff0000008000000) | sign;
            state.fpr[3].ps0 = UINT64_C(0x3ff0000000000000) | sign;
            state.fpscr = mode;
            PpcFloatFused(state, 4, 1, 2, 3, true, true, false);
            Require(state.fpr[4].ps0 == (UINT64_C(0x3e70000000000000) | sign),
                    "Gekko's 25-bit multiplier midpoint did not round magnitude upward independently of RN/sign.");
        }
    }
    for (const auto bits : {UINT64_C(0x7ff0000000001234), UINT64_C(0xfff8000023456789),
                            UINT64_C(0x7ff0000000000000), UINT64_C(0xfff0000000000000),
                            UINT64_C(0), UINT64_C(0x8000000000000000)}) {
        state.fpr[1].ps0 = bits;
        state.fpscr = 0;
        PpcFloatRoundSingle(state, 4, 1);
        const bool nan = (bits & UINT64_C(0x7fffffffffffffff)) > UINT64_C(0x7ff0000000000000);
        const auto expected = nan ? ppc_widen_single_bits(ppc_narrow_single_bits(bits | UINT64_C(0x8000000000000))) : bits;
        Require(state.fpr[4].ps0 == expected &&
                    ((state.fpscr & 0x01000000u) != 0) == (nan && !(bits & UINT64_C(0x8000000000000))),
                "frsp NaN payload/quieting/invalid flag or infinity/signed zero differs.");
    }
    std::mt19937_64 random(0x25b175eed);
    for (unsigned index = 0; index < 20000; ++index) {
        const auto a = Bits<float>(0x3f000000u | uint32_t(random() & 0x7fffffu));
        const auto c = Bits<double>(UINT64_C(0x3ff0000000000000) | (random() & UINT64_C(0x000fffffffffffff)));
        const double rounded = std::floor(c * 16777216.0 + 0.5) / 16777216.0;
        const double product = double(a) * rounded;
        state.fpr[1].ps0 = ppc_widen_single_bits(Bits<uint32_t>(a));
        state.fpr[2].ps0 = Bits<uint64_t>(c);
        state.fpr[3].ps0 = 0x3ff0000000000000;
        state.fpscr = 0;
        PpcFloatArithmetic(state, 4, 1, 2, PpcFloatOperation::Multiply, true);
        Require(state.fpr[4].ps0 == ppc_widen_single_bits(Bits<uint32_t>(float(product))),
                "25-bit multiplier rounding differs from independent exact dyadic multiplication.");
        state.fpscr = 0;
        PpcFloatFused(state, 4, 1, 2, 3, true, true, false);
        Require(state.fpr[4].ps0 == ppc_widen_single_bits(Bits<uint32_t>(float(product - 1.0))),
                "Fused operation did not use the same 25-bit multiplier rounding.");
        state.fpr[1].ps0 = Bits<uint64_t>(c);
        state.fpscr = 0;
        PpcFloatRoundSingle(state, 4, 1);
        Require(state.fpr[4].ps0 == ppc_widen_single_bits(Bits<uint32_t>(float(c))),
                "frsp differs from strict binary64-to-binary32 conversion.");
    }
    for (const uint32_t mode : {4u, 8u, 16u, 32u, 64u, 128u}) {
        state.fpscr = mode;
        state.fpr[1].ps0 = 1;
        const auto before = state.fpr;
        bool rejected = false;
        try { PpcFloatRoundSingle(state, 1, 1); }
        catch (const std::runtime_error&) { rejected = true; }
        Require(rejected && state.fpr == before && state.fpscr == mode,
                "Unsupported frsp NI/exception mode did not fail atomically.");
    }
}

void TestDifferentialFinite() {
    PpcState state;
    uint64_t seed = 0xdead1234beef5678;
    for (unsigned index = 0; index < 100000; ++index) {
        auto a = Random(seed), b = Random(seed);
        if (((a >> 52) & 2047) == 2047 || ((b >> 52) & 2047) == 2047) continue;
        for (const auto op : {PpcFloatOperation::Add, PpcFloatOperation::Subtract,
                              PpcFloatOperation::Multiply, PpcFloatOperation::Divide}) {
            const double x = Bits<double>(a), y = Bits<double>(b);
            volatile double expected = op == PpcFloatOperation::Add ? x + y :
                op == PpcFloatOperation::Subtract ? x - y :
                op == PpcFloatOperation::Multiply ? x * y : x / y;
            const auto actual = Execute(state, a, b, op, false);
            if (actual != Bits<uint64_t>(double(expected))) {
                std::cerr << "Differential double mismatch op=" << unsigned(op) << " a=" << std::hex
                          << a << " b=" << b << " actual=" << actual
                          << " expected=" << Bits<uint64_t>(double(expected)) << std::dec << '\n';
                throw std::runtime_error("Software arithmetic differs from independent IEEE finite double reference.");
            }
        }
        const auto fa = static_cast<uint32_t>(Random(seed)), fb = static_cast<uint32_t>(Random(seed));
        if ((fa & 0x7f800000) == 0x7f800000 || (fb & 0x7f800000) == 0x7f800000) continue;
        const float x = Bits<float>(fa), y = Bits<float>(fb);
        for (const auto op : {PpcFloatOperation::Add, PpcFloatOperation::Subtract,
                              PpcFloatOperation::Multiply, PpcFloatOperation::Divide}) {
            volatile float expected = op == PpcFloatOperation::Add ? x + y :
                op == PpcFloatOperation::Subtract ? x - y :
                op == PpcFloatOperation::Multiply ? x * y : x / y;
            Require(Execute(state, ppc_widen_single_bits(fa), ppc_widen_single_bits(fb), op) ==
                        ppc_widen_single_bits(Bits<uint32_t>(float(expected))),
                    "Software arithmetic differs from independent IEEE finite single reference.");
        }
    }
}

} // namespace

int main() {
    try {
        TestExactAndFlags();
        TestSpecialAndRejectedModes();
        TestCompareAndConversion();
        TestPairedArithmetic();
        TestFusedAndSums();
        TestReciprocalSqrtEstimate();
        TestReciprocalEstimate();
        TestRoundSingleAndMultiplier();
        TestDifferentialFinite();
    } catch (const std::exception& error) {
        std::cerr << "PowerPC float test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PowerPC scalar/paired add/subtract/multiply/divide, broadcast multiply, fused multiply-add/subtract, "
                 "horizontal sums, comparisons/conversion, atomic lane failures and rounding/flags/special values passed; "
                 "100000 finite single/double arithmetic and 20000 fused reference iterations passed.\n";
}
