#include "ppc_float.h"
#include "ppc_state.h"

#include <algorithm>
#include <array>
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
std::pair<uint64_t, uint64_t> WideProduct(uint64_t a, uint64_t b) {
    const uint64_t a0 = static_cast<uint32_t>(a), a1 = a >> 32;
    const uint64_t b0 = static_cast<uint32_t>(b), b1 = b >> 32;
    const auto p0 = a0 * b0, p1 = a0 * b1, p2 = a1 * b0;
    const auto middle = (p0 >> 32) + static_cast<uint32_t>(p1) + static_cast<uint32_t>(p2);
    const auto low = (middle << 32) | static_cast<uint32_t>(p0);
    const auto high = a1 * b1 + (p1 >> 32) + (p2 >> 32) + (middle >> 32);
    return {low, high};
}

uint64_t Product(uint64_t a, uint64_t b, int& exponent) {
    const auto product = WideProduct(a, b);
    const auto low = product.first, high = product.second;
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

uint64_t RoundMultiplier25(uint64_t bits) {
    auto value = Unpack(bits);
    if (value.special || value.significand == 0) return bits;
    auto significand = ((value.significand >> 28) + ((value.significand >> 27) & 1)) << 28;
    if (significand == Hidden * 2) {
        significand >>= 1;
        ++value.exponent;
    }
    return Round(value.negative, value.exponent, significand << 3, false, 0).bits;
}

Result Arithmetic(uint64_t aBits, uint64_t bBits, PpcFloatOperation operation,
                  bool single, uint32_t fpscr) {
    if ((fpscr & 0xf8u) != 0) {
        throw std::runtime_error("PowerPC arithmetic with enabled FP exceptions is not implemented.");
    }
    auto a = Unpack(aBits), b = Unpack(bBits);
    const bool nonIeee = (fpscr & 4) != 0;
    const int minimum = single ? -126 : -1022;
    if (nonIeee && ((!a.special && a.significand && a.exponent < minimum) ||
                    (!b.special && b.significand && b.exponent < minimum))) {
        throw std::runtime_error("PowerPC non-IEEE denormal operand arithmetic is not implemented.");
    }
    if (single && operation == PpcFloatOperation::Multiply) {
        bBits = RoundMultiplier25(bBits);
        b = Unpack(bBits);
    }
    const unsigned rounding = fpscr & 3u;
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
    return result;
}

// Exact product plus addend spans at most 3276 bits for finite binary64 operands.
using Exact = std::array<uint64_t, 52>;

void Place(Exact& value, uint64_t word, unsigned shift) {
    const auto limb = shift / 64, bit = shift % 64;
    value[limb] |= word << bit;
    if (bit != 0) value[limb + 1] |= word >> (64 - bit);
}

Result Fused(uint64_t aBits, uint64_t cBits, uint64_t bBits, bool subtract,
             bool single, uint32_t fpscr) {
    if (fpscr & 0xf8u) {
        throw std::runtime_error("PowerPC fused arithmetic with enabled FP exceptions is not implemented.");
    }
    auto a = Unpack(aBits), b = Unpack(bBits), c = Unpack(cBits);
    if ((fpscr & 4) && ((!a.special && a.significand && a.exponent < (single ? -126 : -1022)) ||
                       (!b.special && b.significand && b.exponent < (single ? -126 : -1022)) ||
                       (!c.special && c.significand && c.exponent < (single ? -126 : -1022)))) {
        throw std::runtime_error("PowerPC non-IEEE denormal fused operands are not implemented.");
    }
    if (single) {
        cBits = RoundMultiplier25(cBits);
        c = Unpack(cBits);
    }
    Result result{0};
    if (a.nan || b.nan || c.nan) {
        result.bits = (a.nan ? aBits : b.nan ? bBits : cBits) | Quiet;
        if (single) result.bits = ppc_widen_single_bits(ppc_narrow_single_bits(result.bits));
        if (a.signaling || b.signaling || c.signaling) result.flags = 0x01000000u;
        return result;
    }
    const bool productSign = a.negative != c.negative;
    const bool addendSign = b.negative != subtract;
    if ((a.special && c.significand == 0) || (c.special && a.significand == 0)) {
        return {UINT64_C(0x7ff8000000000000), 0x00100000u};
    }
    if ((a.special || c.special) && b.special && productSign != addendSign) {
        return {UINT64_C(0x7ff8000000000000), 0x00800000u};
    }
    if (a.special || b.special || c.special) {
        return {((a.special || c.special ? productSign : addendSign) ? Sign : 0) |
                UINT64_C(0x7ff0000000000000)};
    }
    const auto product = WideProduct(a.significand, c.significand);
    int productExponent = a.exponent + c.exponent - 104;
    int addendExponent = b.exponent - 52;
    if (a.significand == 0 || c.significand == 0) productExponent = addendExponent;
    if (b.significand == 0) addendExponent = productExponent;
    const int base = std::min(productExponent, addendExponent);
    Exact x{}, y{};
    Place(x, product.first, unsigned(productExponent - base));
    Place(x, product.second, unsigned(productExponent - base) + 64);
    Place(y, b.significand, unsigned(addendExponent - base));
    bool negative = productSign;
    if (productSign != addendSign &&
        std::lexicographical_compare(x.rbegin(), x.rend(), y.rbegin(), y.rend())) {
        std::swap(x, y);
        negative = addendSign;
    }
    uint64_t carry = 0;
    for (size_t limb = 0; limb < x.size(); ++limb) {
        const auto original = x[limb];
        if (productSign == addendSign) {
            const auto sum = original + y[limb];
            x[limb] = sum + carry;
            carry = (sum < original) || (x[limb] < sum);
        } else {
            const auto difference = original - y[limb];
            x[limb] = difference - carry;
            carry = (original < y[limb]) || (difference < carry);
        }
    }
    int highest = int(x.size() * 64) - 1;
    while (highest >= 0 && ((x[highest / 64] >> (highest % 64)) & 1u) == 0) --highest;
    if (highest < 0) {
        return {(productSign == addendSign ? productSign : (fpscr & 3u) == 3) ? Sign : 0};
    }
    uint64_t extended = 0;
    const int shift = highest - 55;
    for (int bit = 0; bit <= highest; ++bit) {
        if (((x[bit / 64] >> (bit % 64)) & 1u) == 0) continue;
        if (bit < shift) extended |= 1;
        else extended |= UINT64_C(1) << (bit - shift);
    }
    result = Round(negative, base + highest, extended, single, fpscr & 3u);
    if ((fpscr & 4) && (result.subnormal || (result.flags & 0x08000000u))) {
        throw std::runtime_error("PowerPC non-IEEE denormal fused result is not implemented.");
    }
    return result;
}

void CommitStatus(PpcState& state, const Result& result, uint32_t flags) {
    uint32_t fpscr = state.fpscr & ~0x0007f000u;
    fpscr |= Classify(result) << 12;
    if (result.inexact) fpscr |= 0x00020000u;
    if (result.incremented) fpscr |= 0x00040000u;
    if ((flags & ~state.fpscr) != 0) fpscr |= 0x80000000u;
    ppc_write_fpscr(state, fpscr | flags, UINT32_MAX);
}

void CommitPairedStatus(PpcState& state, Result first, const Result& second) {
    first.inexact = first.inexact || second.inexact;
    first.incremented = first.incremented || second.incremented;
    CommitStatus(state, first, first.flags | second.flags);
}

} // namespace

void PpcFloatArithmetic(PpcState& state, unsigned destination, unsigned left,
                        unsigned right, PpcFloatOperation operation, bool single) {
    if (destination >= 32 || left >= 32 || right >= 32) {
        throw std::runtime_error("Invalid PowerPC floating-point register.");
    }
    const auto result = Arithmetic(state.fpr[left].ps0, state.fpr[right].ps0,
                                   operation, single, state.fpscr);
    CommitStatus(state, result, result.flags);
    state.fpr[destination].ps0 = result.bits;
    if (single) state.fpr[destination].ps1 = result.bits;
}

void PpcPairedArithmetic(PpcState& state, unsigned destination, unsigned left,
                         unsigned right, PpcFloatOperation operation, int broadcast) {
    if (destination >= 32 || left >= 32 || right >= 32 || broadcast < -1 || broadcast > 1) {
        throw std::runtime_error("Invalid PowerPC paired arithmetic register/lane.");
    }
    const auto a = state.fpr[left], b = state.fpr[right];
    const auto first = Arithmetic(a.ps0, broadcast == 1 ? b.ps1 : b.ps0,
                                  operation, true, state.fpscr);
    const auto second = Arithmetic(a.ps1, broadcast == 0 ? b.ps0 : b.ps1,
                                   operation, true, state.fpscr);
    CommitPairedStatus(state, first, second);
    state.fpr[destination] = {first.bits, second.bits};
}

void PpcFloatFused(PpcState& state, unsigned destination, unsigned left, unsigned multiplier,
                   unsigned addend, bool subtract, bool single, bool paired, bool negate) {
    if (destination >= 32 || left >= 32 || multiplier >= 32 || addend >= 32 || (paired && !single)) {
        throw std::runtime_error("Invalid PowerPC fused arithmetic register/precision.");
    }
    const auto a = state.fpr[left], b = state.fpr[addend], c = state.fpr[multiplier];
    auto first = Fused(a.ps0, c.ps0, b.ps0, subtract, single, state.fpscr);
    auto second = paired ? Fused(a.ps1, c.ps1, b.ps1, subtract, true, state.fpscr) : first;
    if (negate) {
        if (!Unpack(first.bits).nan) first.bits ^= Sign;
        if (!Unpack(second.bits).nan) second.bits ^= Sign;
    }
    CommitPairedStatus(state, first, second);
    state.fpr[destination].ps0 = first.bits;
    if (single) state.fpr[destination].ps1 = second.bits;
}

void PpcFloatReciprocalSqrtEstimate(PpcState& state, unsigned destination, unsigned source) {
    if (destination >= 32 || source >= 32) {
        throw std::runtime_error("Invalid PowerPC reciprocal square-root register.");
    }
    if (state.fpscr & 0xf8u) {
        throw std::runtime_error("PowerPC reciprocal square-root enabled exceptions are not implemented.");
    }
    const auto bits = state.fpr[source].ps0;
    const auto value = Unpack(bits);
    Result result{0};
    if (value.nan) {
        result.bits = bits | Quiet;
        if (value.signaling) result.flags = 0x01000000u;
    } else if (value.significand == 0 && !value.special) {
        result.bits = (bits & Sign) | UINT64_C(0x7ff0000000000000);
        result.flags = 0x04000000u;
    } else if (value.negative) {
        result.bits = UINT64_C(0x7ff8000000000000);
        result.flags = 0x00000200u;
    } else if (!value.special) {
        if ((state.fpscr & 4) && value.exponent < -1022) {
            throw std::runtime_error("PowerPC non-IEEE denormal reciprocal square-root input is not implemented.");
        }
        // Measured Gekko estimate ROM base/decrement data; see ORIGINAL_STARTUP.md.
        constexpr uint32_t bases[] = {
            0x1a7e800, 0x17cb800, 0x1552800, 0x130c000, 0x10f2000, 0x0eff000, 0x0d2e000, 0x0b7c000,
            0x09e5000, 0x0867000, 0x06ff000, 0x05ab800, 0x046a000, 0x0339800, 0x0218800, 0x0105800,
            0x3ffa000, 0x3c29000, 0x38aa000, 0x3572000, 0x3279000, 0x2fb7000, 0x2d26000, 0x2ac0000,
            0x2881000, 0x2665000, 0x2468000, 0x2287000, 0x20c1000, 0x1f12000, 0x1d79000, 0x1bf4000
        };
        constexpr uint32_t decrements[] = {
            0x568, 0x4f3, 0x48d, 0x435, 0x3e7, 0x3a2, 0x365, 0x32e,
            0x2fc, 0x2d0, 0x2a8, 0x283, 0x261, 0x243, 0x226, 0x20b,
            0x7a4, 0x700, 0x670, 0x5f2, 0x584, 0x524, 0x4cc, 0x47e,
            0x43a, 0x3fa, 0x3c2, 0x38e, 0x35e, 0x332, 0x30a, 0x2e6
        };
        const unsigned fractionIndex = unsigned((value.significand & Fraction) >> 37);
        const unsigned row = fractionIndex / 2048 + ((value.exponent % 2 == 0) ? 16 : 0);
        const uint32_t fraction = bases[row] - decrements[row] * (fractionIndex % 2048);
        const int halfExponent = value.exponent >= 0 ? value.exponent / 2 : (value.exponent - 1) / 2;
        const int resultExponent = -halfExponent - 1;
        result.bits = (uint64_t(resultExponent + 1023) << 52) | (uint64_t(fraction) << 26);
    }
    CommitStatus(state, result, result.flags);
    state.fpr[destination].ps0 = result.bits;
}

void PpcFloatReciprocalEstimate(PpcState& state, unsigned destination, unsigned source) {
    if (destination >= 32 || source >= 32) {
        throw std::runtime_error("Invalid PowerPC reciprocal estimate register.");
    }
    if (state.fpscr & 0xf8u) {
        throw std::runtime_error("PowerPC reciprocal estimate enabled exceptions are not implemented.");
    }
    const auto bits = state.fpr[source].ps0;
    const auto value = Unpack(bits);
    Result result{bits & Sign};
    const auto exponent = unsigned((bits >> 52) & 0x7ff);
    if (value.nan) {
        result.bits = bits | Quiet;
        if (value.signaling) result.flags = 0x01000000u;
    } else if (!value.special && value.significand == 0) {
        result.bits |= UINT64_C(0x7ff0000000000000);
        result.flags = 0x04000000u;
    } else if (!value.special) {
        if ((state.fpscr & 4) && value.exponent < -1022) {
            throw std::runtime_error("PowerPC non-IEEE denormal reciprocal estimate input is not implemented.");
        }
        if (exponent < 895) {
            result.bits |= UINT64_C(0x47efffffe0000000);
        } else if (exponent < 1149) {
            // Measured Gekko estimate ROM base/decrement data; see ORIGINAL_STARTUP.md.
            constexpr uint32_t bases[] = {
                0x7ff800, 0x783800, 0x70ea00, 0x6a0800, 0x638800, 0x5d6200, 0x579000, 0x520800,
                0x4cc800, 0x47ca00, 0x430800, 0x3e8000, 0x3a2c00, 0x360800, 0x321400, 0x2e4a00,
                0x2aa800, 0x272c00, 0x23d600, 0x209e00, 0x1d8800, 0x1a9000, 0x17ae00, 0x14f800,
                0x124400, 0x0fbe00, 0x0d3800, 0x0ade00, 0x088400, 0x065000, 0x041c00, 0x020c00
            };
            constexpr uint32_t decrements[] = {
                0x3e1, 0x3a7, 0x371, 0x340, 0x313, 0x2ea, 0x2c4, 0x2a0,
                0x27f, 0x261, 0x245, 0x22a, 0x212, 0x1fb, 0x1e5, 0x1d1,
                0x1be, 0x1ac, 0x19b, 0x18b, 0x17c, 0x16e, 0x15b, 0x15b,
                0x143, 0x143, 0x12d, 0x12d, 0x11a, 0x11a, 0x108, 0x106
            };
            const unsigned index = unsigned((bits & Fraction) >> 37);
            const auto fraction = bases[index / 1024] -
                (decrements[index / 1024] * (index % 1024) + 1) / 2;
            result.bits |= (uint64_t(2045 - exponent) << 52) | (uint64_t(fraction) << 29);
        }
    }
    const auto preserved = !value.special && value.significand ? state.fpscr & 0x60000u : 0;
    CommitStatus(state, result, result.flags);
    state.fpscr |= preserved;
    state.fpr[destination] = {result.bits, result.bits};
}

void PpcFloatRoundSingle(PpcState& state, unsigned destination, unsigned source) {
    if (destination >= 32 || source >= 32) {
        throw std::runtime_error("Invalid PowerPC round-to-single register.");
    }
    if (state.fpscr & 0xf8u) {
        throw std::runtime_error("PowerPC round-to-single enabled exceptions are not implemented.");
    }
    const auto bits = state.fpr[source].ps0;
    const auto value = Unpack(bits);
    if ((state.fpscr & 4) && !value.special && value.significand && value.exponent < -126) {
        throw std::runtime_error("PowerPC non-IEEE denormal round-to-single input is not implemented.");
    }
    Result result = value.special ? Result{value.nan ? ppc_widen_single_bits(ppc_narrow_single_bits(bits | Quiet)) : bits,
                                           value.signaling ? 0x01000000u : 0u} :
        Round(value.negative, value.exponent, value.significand << 3, true, state.fpscr & 3);
    if ((state.fpscr & 4) && result.subnormal) {
        throw std::runtime_error("PowerPC non-IEEE denormal round-to-single result is not implemented.");
    }
    CommitStatus(state, result, result.flags);
    state.fpr[destination].ps0 = result.bits;
}

void PpcPairedSum(PpcState& state, unsigned destination, unsigned left, unsigned right,
                  unsigned copied, bool sumInSecond) {
    if (destination >= 32 || left >= 32 || right >= 32 || copied >= 32) {
        throw std::runtime_error("Invalid PowerPC paired sum register.");
    }
    const auto copy = sumInSecond ? state.fpr[copied].ps0 : state.fpr[copied].ps1;
    const auto sum = Arithmetic(state.fpr[left].ps0, state.fpr[right].ps1,
                                PpcFloatOperation::Add, true, state.fpscr);
    CommitStatus(state, sum, sum.flags);
    state.fpr[destination] = sumInSecond ? PpcFpr{copy, sum.bits} : PpcFpr{sum.bits, copy};
}

void PpcFloatCompare(PpcState& state, unsigned field, unsigned left, unsigned right,
                     bool ordered, bool secondLane) {
    if (field >= 8 || left >= 32 || right >= 32) {
        throw std::runtime_error("Invalid PowerPC floating-point comparison register.");
    }
    if ((state.fpscr & 0xf8u) != 0) {
        throw std::runtime_error("PowerPC comparison with enabled FP exceptions is not implemented.");
    }
    const auto x = secondLane ? state.fpr[left].ps1 : state.fpr[left].ps0;
    const auto y = secondLane ? state.fpr[right].ps1 : state.fpr[right].ps0;
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
