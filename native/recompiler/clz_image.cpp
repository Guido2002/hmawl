#include "clz_image.h"

#include <stdexcept>

namespace {

uint32_t Read32(const std::vector<uint8_t>& input, size_t offset) {
    if (offset > input.size() || input.size() - offset < 4) {
        throw std::runtime_error("CLZ header or block size is truncated.");
    }
    return (uint32_t{input[offset]} << 24) | (uint32_t{input[offset + 1]} << 16) |
           (uint32_t{input[offset + 2]} << 8) | input[offset + 3];
}

} // namespace

std::vector<uint8_t> DecodeClz(const std::vector<uint8_t>& input, size_t maximumOutputSize) {
    if (Read32(input, 0) != 0x434c5a00u) {
        throw std::runtime_error("Unsupported CLZ signature or version.");
    }
    const uint32_t size = Read32(input, 4);
    const uint32_t stride = Read32(input, 8);
    if (size > maximumOutputSize) {
        throw std::runtime_error("CLZ output exceeds the configured size limit.");
    }
    std::vector<uint8_t> output;
    output.reserve(size);
    size_t block = 12;
    while (output.size() < size) {
        const uint32_t blockSize = Read32(input, block);
        if (blockSize == 0 || blockSize > size - output.size()) {
            throw std::runtime_error("CLZ block has an invalid output size.");
        }
        const size_t end = output.size() + blockSize;
        const bool more = end < size;
        size_t next = input.size();
        if (more) {
            const size_t advance = block == 12 ? (stride >= 12 ? stride - 12u : 0u) : stride;
            if (advance < 4 || advance > input.size() - block) {
                throw std::runtime_error("CLZ block stride is invalid or truncated.");
            }
            next = block + advance;
        }
        size_t cursor = block + 4;
        const auto byte = [&]() -> uint8_t {
            if (cursor >= next) {
                throw std::runtime_error("CLZ token is truncated or crosses its block stride.");
            }
            return input[cursor++];
        };
        while (output.size() < end) {
            const uint8_t flags = byte();
            for (unsigned bit = 0; bit < 8 && output.size() < end; ++bit) {
                if ((flags & (1u << bit)) == 0) {
                    output.push_back(byte());
                } else {
                    const uint32_t low = byte();
                    const uint32_t high = byte();
                    const uint32_t distance = 4096u - (low | ((high & 0xf0u) << 4));
                    const uint32_t length = (high & 15u) + 3u;
                    if (distance > output.size() || length > end - output.size()) {
                        throw std::runtime_error("CLZ back-reference is outside its decoded window or block.");
                    }
                    for (uint32_t i = 0; i < length; ++i) {
                        output.push_back(output[output.size() - distance]);
                    }
                }
            }
        }
        block = next;
    }
    return output;
}
