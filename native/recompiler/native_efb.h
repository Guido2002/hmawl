#ifndef HMAWL_NATIVE_EFB_H
#define HMAWL_NATIVE_EFB_H

#include <cstdint>
#include <optional>
#include <vector>

struct PpcState;

struct NativeXfbCopy {
    uint32_t address = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t undefinedPixels = 0;
};

struct NativeEfb {
    static constexpr uint32_t width = 640;
    static constexpr uint32_t height = 528;
    // Power-on EFB contents are unspecified, not a rendered or cleared image.
    std::vector<uint32_t> color = std::vector<uint32_t>(width * height, 0xcdcdcdffu);
    std::vector<uint32_t> depth = std::vector<uint32_t>(width * height, 0xcdcdcdu);
    std::vector<uint8_t> defined = std::vector<uint8_t>(width * height);
    std::optional<NativeXfbCopy> lastCopy;
    uint64_t copies = 0;
    uint64_t clears = 0;
    uint64_t undefinedCopies = 0;
};

void NativeEfbCopy(PpcState& state, uint32_t control);

#endif
