#include "native_efb.h"
#include "ppc_state.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace {

struct Sample {
    std::array<int, 3> rgb{};
    bool defined = true;
};

uint8_t Yuv(int value, int bias, int denominator = 256) {
    // Round signed fixed-point values without implementation-defined right shifts.
    const int rounded = value + denominator / 2;
    const int quotient = rounded >= 0 ? rounded / denominator :
                                      -((-rounded + denominator - 1) / denominator);
    return static_cast<uint8_t>(std::clamp(quotient + bias, 0, 255));
}

bool Overlaps(uint64_t start, uint64_t size, uint32_t base, uint32_t end) {
    return start < uint64_t(end) + 32 && uint64_t(base) < start + size;
}

} // namespace

void NativeEfbCopy(PpcState& state, uint32_t control) {
    auto& gx = state.graphics.value();
    if (!gx.efb) throw std::runtime_error("Native GX EFB copy requires a renderer/copy backend.");
    auto& efb = *gx.efb;
    const auto& bp = gx.bp;
    if ((control & 0x4000u) == 0) {
        throw std::runtime_error("Native GX texture EFB copy requires a texture-memory backend.");
    }
    if ((control & ~0x4c03u) != 0 || (bp[0x43] & ~0x40u) != 0 ||
        (bp[0] & 0x200u) != 0) {
        throw std::runtime_error("Native EFB copy supports only non-AA RGB8/Z24 XFB copies, gamma 1 and no field/half scaling.");
    }
    if ((bp[0x4e] & ~0x1ffu) != 0 || bp[0x4e] != 256) {
        throw std::runtime_error("Native EFB XFB vertical scaling other than 1:1 is not implemented.");
    }
    const uint32_t x = bp[0x49] & 0x3ffu, y = (bp[0x49] >> 10) & 0x3ffu;
    const uint32_t width = (bp[0x4a] & 0x3ffu) + 1, height = ((bp[0x4a] >> 10) & 0x3ffu) + 1;
    if ((bp[0x49] >> 20) != 0 || (bp[0x4a] >> 20) != 0 ||
        x >= NativeEfb::width || width > NativeEfb::width - x ||
        y >= NativeEfb::height || height > NativeEfb::height - y ||
        (x & 1u) || (width & 1u)) {
        throw std::runtime_error("Native EFB copy rectangle exceeds storage or has unsupported odd X/width.");
    }
    if ((bp[0x4b] & ~0x1fffffu) != 0 || (bp[0x4d] & ~0x3ffu) != 0) {
        throw std::runtime_error("Invalid native EFB copy address/stride bits.");
    }
    const uint32_t address = bp[0x4b] << 5, stride = bp[0x4d] << 5;
    if (stride < width * 2) throw std::runtime_error("Native EFB XFB stride is smaller than the copied row.");
    const uint64_t bytes = uint64_t(height - 1) * stride + width * 2;
    const auto destination = ppc_memory_offset(state, address, static_cast<size_t>(bytes));
    if (Overlaps(address, bytes, gx.cpuBase, gx.cpuEnd) ||
        Overlaps(address, bytes, gx.fifo[0], gx.fifo[1])) {
        throw std::runtime_error("Native EFB XFB copy cannot overwrite its command FIFO.");
    }
    const unsigned weights[] = {
        (bp[0x53] & 63u) + ((bp[0x53] >> 6) & 63u),
        ((bp[0x53] >> 12) & 63u) + ((bp[0x53] >> 18) & 63u) + (bp[0x54] & 63u),
        ((bp[0x54] >> 6) & 63u) + ((bp[0x54] >> 12) & 63u)
    };
    if ((bp[0x54] >> 18) != 0 || weights[0] + weights[1] + weights[2] > 64) {
        throw std::runtime_error("Native EFB copy filter overflow is not implemented.");
    }
    const bool clear = (control & 0x800u) != 0;
    if (clear && ((bp[0x43] & ~0x47u) != 0 || (bp[0x4f] >> 16) != 0 ||
                  (bp[0x50] >> 16) != 0)) {
        throw std::runtime_error("Unsupported native EFB clear configuration.");
    }
    const auto pixels = size_t(NativeEfb::width) * NativeEfb::height;
    if (efb.color.size() != pixels || efb.depth.size() != pixels || efb.defined.size() != pixels) {
        throw std::runtime_error("Native EFB storage dimensions are invalid.");
    }
    const auto sample = [&](uint32_t column, uint32_t row) {
        Sample result;
        for (unsigned tap = 0; tap < 3; ++tap) {
            if (weights[tap] == 0) continue;
            int sourceRow = int(y + row) + int(tap) - 1;
            if (sourceRow < int(y) && (control & 1u)) sourceRow = int(y);
            if (sourceRow >= int(y + height) && (control & 2u)) sourceRow = int(y + height) - 1;
            sourceRow = std::clamp(sourceRow, 0, int(NativeEfb::height) - 1);
            const auto index = size_t(sourceRow) * NativeEfb::width + x + column;
            const auto color = efb.color[index];
            for (unsigned channel = 0; channel < 3; ++channel) {
                result.rgb[channel] += int((color >> (24 - channel * 8)) & 255u) * int(weights[tap]);
            }
            result.defined = result.defined && (efb.defined[index] & 1u) != 0;
        }
        for (auto& channel : result.rgb) channel /= 64;
        return result;
    };
    // Validate the entire request before touching guest XFB RAM or EFB pixels.
    uint32_t undefinedPixels = 0;
    for (uint32_t row = 0; row < height; ++row) {
        auto output = state.memory.begin() + destination + size_t(row) * stride;
        for (uint32_t column = 0; column < width; column += 2) {
            const auto a = sample(column, row), b = sample(column + 1, row);
            const bool defined = a.defined && b.defined;
            undefinedPixels += defined ? 0 : 2;
            const auto& c = a.rgb;
            const auto& d = b.rgb;
            // BT.601 limited-range Y0/Cb/Y1/Cr, chroma shared by each horizontal pair.
            *output++ = Yuv(66 * c[0] + 129 * c[1] + 25 * c[2], 16);
            *output++ = Yuv(-38 * (c[0] + d[0]) - 74 * (c[1] + d[1]) +
                             112 * (c[2] + d[2]), 128, 512);
            *output++ = Yuv(66 * d[0] + 129 * d[1] + 25 * d[2], 16);
            *output++ = Yuv(112 * (c[0] + d[0]) - 94 * (c[1] + d[1]) -
                             18 * (c[2] + d[2]), 128, 512);
        }
    }
    efb.lastCopy = NativeXfbCopy{address, width, height, stride, undefinedPixels};
    ++efb.copies;
    if (undefinedPixels) ++efb.undefinedCopies;
    if (clear) {
        const uint32_t color = ((bp[0x4f] & 255u) << 24) | ((bp[0x50] & 0xffffu) << 8) | 255u;
        const bool colorUpdate = (bp[0x41] & 8u) != 0, depthUpdate = (bp[0x40] & 16u) != 0;
        for (uint32_t row = 0; row < height; ++row) {
            for (uint32_t column = 0; column < width; ++column) {
                const auto index = size_t(y + row) * NativeEfb::width + x + column;
                if (colorUpdate) {
                    efb.color[index] = color;
                    efb.defined[index] |= 1;
                }
                if (depthUpdate) {
                    efb.depth[index] = bp[0x51];
                    efb.defined[index] |= 2;
                }
            }
        }
        ++efb.clears;
    }
}
