#include "ppc_float.h"
#include "ppc_state.h"

#include <stdexcept>
#include <utility>

namespace {

constexpr uint64_t Sign = UINT64_C(0x8000000000000000);
constexpr uint64_t Fraction = UINT64_C(0x000fffffffffffff);
constexpr uint64_t Hidden = UINT64_C(0x0010000000000000);
constexpr uint64_t Quiet = UINT64_C(0x0008000000000000);

struct Number {
    bool negative;
    int exponent;
    uint64_t significand;
    bool special;
    bool nan;
    bool signaling;
};

Number Unpack(uint64_t bits) {
    const auto exponent = static_cast<unsigned>((bits >> 52) & 2047);
    Number value{(bits & Sign) != 0, static_cast<int>(exponent) - 1023,
                 bits & Fraction, exponent == 2047, false, false};
    value.nan = value.special && value.significand != 0;
    value.signaling = value.nan && (value.significand & Quiet) == 0;
    if (exponent != 0) value.significand |= Hidden;
    else {
        value.exponent = -1022;
        if (value.significand != 0) {
            while (value.significand < Hidden) {
                value.significand <<= 1;
                --value.exponent;
            }
        }
    }
    return value;
}

uint64_t ShiftJam(uint64_t value, unsigned shift) {
    if (shift == 0) return value;
    if (shift >= 64) return value != 0;
    return (value >> shift) | ((value << (64 - shift)) != 0);
}

// The 53x53-bit product fits in two portable 64-bit limbs, including on MSVC.
uint64_t Product(uint64_t a, uint64_t b, int& exponent) {
    const uint64_t a0 = static_cast<uint32_t>(a), a1 = a >> 32;
    const uint64_t b0 = static_cast<uint32_t>(b), b1 = b >> 32;
    const auto p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0;
    const auto middle = (p0 >> 32) + static_cast<uint32_t>(p1) + static_cast<uint32_t>(p2);
    const auto low = (middle << 32) | static_cast<uint32_t>(p0);
    const auto high = a1 * b1 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    const unsigned shift = (high & (UINT64_C(1) << 41)) != 0 ? 50 : 49;
    if (shift == 50) ++exponent;
    return (high << (64 - shift)) | (low >> shift) |
           ((low & ((UINT64_C(1) << shift) - 1)) != 0);
}

struct Result {
    uint64_t bits;
    uint32_t flags = 0;
    bool inexact = false;
    bool incremented = false;
    bool subnormal = false;
};

Result Round(bool negative, int exponent, uint64_t extended, bool single, unsigned mode) {
    if (extended == 0) return {negative ? Sign : 0};
    const unsigned precision = single ? 24 : 53;
    const int minimum = single ? -126 : -1022;
    const int maximum = single ? 127 : 1023;
    const uint64_t hidden = UINT64_C(1) << (precision - 1);
    // extended has a leading bit at 55 and three guard/round/sticky bits.
    if (single) extended = ShiftJam(extended, 29);
    if (exponent < minimum) {
        extended = ShiftJam(extended, static_cast<unsigned>(minimum - exponent));
        exponent = minimum;
    }
    auto significand = extended >> 3;
    const unsigned remainder = static_cast<unsigned>(extended & 7);
    const bool increment = mode == 0 ? remainder > 4 || (remainder == 4 && (significand & 1) != 0) :
        mode == 1 ? false : remainder != 0 && (mode == 2 ? !negative : negative);
    if (increment) ++significand;
    if (significand >= hidden * 2) {
        significand >>= 1;
        ++exponent;
    }
    Result result{negative ? Sign : 0, 0, remainder != 0, increment};
    if (exponent > maximum) {
        const bool infinity = mode == 0 || (mode == 2 && !negative) || (mode == 3 && negative);
        result.bits |= infinity ? UINT64_C(0x7ff0000000000000) :
            single ? ppc_widen_single_bits(0x7f7fffff) : UINT64_C(0x7fefffffffffffff);
        result.flags = 0x12000000u;
        result.inexact = true;
        result.incremented = infinity;
        return result;
    }
    result.subnormal = significand != 0 && significand < hidden;
    if (result.inexact) {
        result.flags |= 0x02000000u;
        if (significand < hidden) result.flags |= 0x08000000u;
    }
    if (single) {
        const auto field = significand >= hidden ? static_cast<uint32_t>(exponent + 127) : 0;
        result.bits = ppc_widen_single_bits((negative ? 0x80000000u : 0) |
            (field << 23) | (static_cast<uint32_t>(significand) & 0x7fffffu));
    } else {
        const auto field = significand >= hidden ? static_cast<uint64_t>(exponent + 1023) : 0;
        result.bits |= (field << 52) | (significand & Fraction);
    }
    return result;
}

uint32_t Classify(const Result& result) {
    const auto bits = result.bits;
    const bool negative = (bits & Sign) != 0;
    const auto magnitude = bits & ~Sign;
    if (magnitude > UINT64_C(0x7ff0000000000000)) return 0x11;
    if (magnitude == UINT64_C(0x7ff0000000000000)) return negative ? 9 : 5;
    if (magnitude == 0) return negative ? 0x12 : 2;
    if (result.subnormal) return negative ? 0x18 : 0x14;
    return negative ? 8 : 4;
}

} // namespace

void PpcFloatArithmetic(PpcState& state, unsigned destination, unsigned left,
                        unsigned right, PpcFloatOperation operation, bool single) {
    if (destination >= 32 || left >= 32 || right >= 32) {
        throw std::runtime_error("Invalid PowerPC floating-point register.");
    }
    if ((state.fpscr & 0xf8u) != 0) {
        throw std::runtime_error("PowerPC arithmetic with enabled FP exceptions is not implemented.");
    }
    const auto aBits = state.fpr[left].ps0, bBits = state.fpr[right].ps0;
    auto a = Unpack(aBits), b = Unpack(bBits);
    const bool nonIeee = (state.fpscr & 4) != 0;
    const int minimum = single ? -126 : -1022;
    if (nonIeee && ((!a.special && a.significand && a.exponent < minimum) ||
                    (!b.special && b.significand && b.exponent < minimum))) {
        throw std::runtime_error("PowerPC non-IEEE denormal operand arithmetic is not implemented.");
    }
    if (single && operation == PpcFloatOperation::Multiply && !b.special &&
        ppc_widen_single_bits(ppc_narrow_single_bits(bBits)) != bBits) {
        throw std::runtime_error("PowerPC fmuls non-single multiplier rounding is not implemented.");
    }
    const unsigned rounding = state.fpscr & 3u;
    Result result{0};
    if (a.nan || b.nan) {
        result.bits = (a.nan ? aBits : bBits) | Quiet;
        if (single) result.bits = ppc_widen_single_bits(ppc_narrow_single_bits(result.bits));
        if (a.signaling || b.signaling) result.flags = 0x01000000u;
    } else if (operation == PpcFloatOperation::Divide) {
        const bool negative = a.negative != b.negative;
        if ((a.special && b.special) || (a.significand == 0 && b.significand == 0)) {
            result.bits = UINT64_C(0x7ff8000000000000);
            result.flags = a.special ? 0x00400000u : 0x00200000u;
        } else if (a.special || b.significand == 0) {
            result.bits = (negative ? Sign : 0) | UINT64_C(0x7ff0000000000000);
            if (!a.special) result.flags = 0x04000000u;
        } else if (b.special || a.significand == 0) {
            result.bits = negative ? Sign : 0;
        } else {
            int exponent = a.exponent - b.exponent;
            uint64_t remainder = a.significand;
            if (remainder < b.significand) {
                remainder <<= 1;
                --exponent;
            }
            remainder -= b.significand;
            uint64_t quotient = 1;
            for (unsigned bit = 0; bit < 55; ++bit) {
                remainder <<= 1;
                quotient <<= 1;
                if (remainder >= b.significand) {
                    remainder -= b.significand;
                    quotient |= 1;
                }
            }
            result = Round(negative, exponent, quotient | (remainder != 0), single, rounding);
        }
    } else if (operation == PpcFloatOperation::Multiply) {
        const bool negative = a.negative != b.negative;
        if ((a.special && b.significand == 0) || (b.special && a.significand == 0)) {
            result.bits = UINT64_C(0x7ff8000000000000);
            result.flags = 0x00100000u;
        } else if (a.special || b.special) {
            result.bits = (negative ? Sign : 0) | UINT64_C(0x7ff0000000000000);
        } else if (a.significand == 0 || b.significand == 0) {
            result.bits = negative ? Sign : 0;
        } else {
            int exponent = a.exponent + b.exponent;
            const auto product = Product(a.significand, b.significand, exponent);
            result = Round(negative, exponent, product, single, rounding);
        }
    } else {
        if (operation == PpcFloatOperation::Subtract) b.negative = !b.negative;
        if (a.special && b.special && a.negative != b.negative) {
            result.bits = UINT64_C(0x7ff8000000000000);
            result.flags = 0x00800000u;
        } else if (a.special || b.special) {
            result.bits = ((a.special ? a.negative : b.negative) ? Sign : 0) |
                          UINT64_C(0x7ff0000000000000);
        } else {
            if (a.significand == 0) a.exponent = b.exponent;
            if (b.significand == 0) b.exponent = a.exponent;
            if (a.exponent < b.exponent ||
                (a.exponent == b.exponent && a.significand < b.significand)) std::swap(a, b);
            const auto x = a.significand << 3;
            const auto y = ShiftJam(b.significand << 3, static_cast<unsigned>(a.exponent - b.exponent));
            uint64_t value = a.negative == b.negative ? x + y : x - y;
            bool negative = a.negative;
            int exponent = a.exponent;
            if (value == 0) {
                negative = a.negative == b.negative ? a.negative : rounding == 3;
            } else if (value >= (Hidden << 4)) {
                value = ShiftJam(value, 1);
                ++exponent;
            } else {
                while (value < (Hidden << 3)) {
                    value <<= 1;
                    --exponent;
                }
            }
            result = Round(negative, exponent, value, single, rounding);
        }
    }
    if (nonIeee && (result.subnormal || (result.flags & 0x08000000u))) {
        throw std::runtime_error("PowerPC non-IEEE denormal result arithmetic is not implemented.");
    }
    // Preserve sticky causes, derive VX/FEX, and set FX only for newly raised causes.
    uint32_t fpscr = state.fpscr & ~0x0007f000u;
    fpscr |= Classify(result) << 12;
    if (result.inexact) fpscr |= 0x00020000u;
    if (result.incremented) fpscr |= 0x00040000u;
    if ((result.flags & ~state.fpscr) != 0) fpscr |= 0x80000000u;
    ppc_write_fpscr(state, fpscr | result.flags, UINT32_MAX);
    state.fpr[destination].ps0 = result.bits;
    if (single) state.fpr[destination].ps1 = result.bits;
}

void PpcFloatCompare(PpcState& state, unsigned field, unsigned left, unsigned right, bool ordered) {
    if (field >= 8 || left >= 32 || right >= 32) {
        throw std::runtime_error("Invalid PowerPC floating-point comparison register.");
    }
    if ((state.fpscr & 0xf8u) != 0) {
        throw std::runtime_error("PowerPC comparison with enabled FP exceptions is not implemented.");
    }
    const auto x = state.fpr[left].ps0, y = state.fpr[right].ps0;
    const auto a = Unpack(x), b = Unpack(y);
    if ((state.fpscr & 4) && ((!a.special && a.significand && a.exponent < -1022) ||
                            (!b.special && b.significand && b.exponent < -1022))) {
        throw std::runtime_error("PowerPC non-IEEE denormal comparison is not implemented.");
    }
    uint32_t condition;
    if (a.nan || b.nan) condition = 1;
    else if (x == y || ((x | y) & ~Sign) == 0) condition = 2;
    else {
        const bool less = a.negative != b.negative ? a.negative : a.negative ? x > y : x < y;
        condition = less ? 8 : 4;
    }
    uint32_t fpscr = (state.fpscr & ~0x0000f000u) | (condition << 12);
    const uint32_t flags = (a.signaling || b.signaling ? 0x01000000u : 0) |
        (ordered && (a.nan || b.nan) ? 0x00080000u : 0);
    if ((flags & ~fpscr) != 0) fpscr |= 0x80000000u;
    fpscr |= flags;
    ppc_write_fpscr(state, fpscr, UINT32_MAX);
    const unsigned shift = 28 - field * 4;
    state.cr = (state.cr & ~(15u << shift)) | (condition << shift);
}

void PpcFloatToInteger(PpcState& state, unsigned destination, unsigned source) {
    if (destination >= 32 || source >= 32) {
        throw std::runtime_error("Invalid PowerPC integer-conversion register.");
    }
    if (state.fpscr & 0xf8u) {
        throw std::runtime_error("PowerPC integer conversion with enabled FP exceptions is not implemented.");
    }
    const auto bits = state.fpr[source].ps0;
    const auto value = Unpack(bits);
    if ((state.fpscr & 4) && value.significand && value.exponent < -1022) {
        throw std::runtime_error("PowerPC non-IEEE denormal integer conversion is not implemented.");
    }
    bool inexact = false;
    uint64_t magnitude = 0;
    if (!value.special && value.significand) {
        if (value.exponent < 0) inexact = true;
        else if (value.exponent <= 31) {
            const unsigned shift = 52 - value.exponent;
            magnitude = value.significand >> shift;
            inexact = (value.significand & ((UINT64_C(1) << shift) - 1)) != 0;
        }
    }
    const uint64_t limit = value.negative ? UINT64_C(0x80000000) : UINT64_C(0x7fffffff);
    const bool invalid = value.special || (value.significand && value.exponent > 31) || magnitude > limit;
    const uint32_t word = invalid ? (value.nan || value.negative ? 0x80000000u : 0x7fffffffu) :
        value.negative ? 0u - static_cast<uint32_t>(magnitude) : static_cast<uint32_t>(magnitude);
    uint32_t fpscr = state.fpscr & ~0x00060000u;
    const uint32_t flags = invalid ? 0x00000100u : inexact ? 0x02000000u : 0;
    if ((flags & ~fpscr) != 0) fpscr |= 0x80000000u;
    if (!invalid && inexact) fpscr |= 0x00020000u;
    ppc_write_fpscr(state, fpscr | flags, UINT32_MAX);
    state.fpr[destination].ps0 = UINT64_C(0xfff8000000000000) | word |
        (word == 0 && value.negative ? UINT64_C(0x0000000100000000) : 0);
}
