#include "ppc_state.h"

#include <cstring>
#include <iostream>
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
    const auto original = state.fpr;
    bool rejected = false;
    try {
        PpcFloatArithmetic(state, 3, 1, 2, Op::Multiply, true);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("multiplier rounding") != std::string::npos;
    }
    Require(rejected && state.fpscr == 0 && state.fpr == original,
            "Non-single multiplier silently skipped Gekko-specific operand rounding.");
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
        TestDifferentialFinite();
    } catch (const std::exception& error) {
        std::cerr << "PowerPC float test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "PowerPC software add/subtract/multiply/divide, comparisons, integer conversion, rounding/flags/special values and "
                 "100000 deterministic finite single/double differential cases passed.\n";
}
