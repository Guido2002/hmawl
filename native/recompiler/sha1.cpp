#include "sha1.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace {

uint32_t Rotate(uint32_t value, unsigned shift) {
    return (value << shift) | (value >> (32u - shift));
}

} // namespace

Sha1Digest Sha1(const uint8_t* bytes, size_t size) {
    if (size > (std::numeric_limits<uint64_t>::max() - 72u) / 8u) {
        throw std::runtime_error("SHA-1 input is too large.");
    }
    uint32_t hash[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu,
                        0x10325476u, 0xc3d2e1f0u};
    const uint64_t length = static_cast<uint64_t>(size);
    const uint64_t blocks = (length + 9u + 63u) / 64u;
    for (uint64_t block = 0; block < blocks; ++block) {
        uint32_t words[80] = {};
        for (size_t i = 0; i < 64; ++i) {
            const uint64_t offset = block * 64u + i;
            uint8_t byte = 0;
            if (offset < length) {
                byte = bytes[static_cast<size_t>(offset)];
            } else if (offset == length) {
                byte = 0x80;
            } else if (offset >= blocks * 64u - 8u) {
                byte = static_cast<uint8_t>(
                    (length * 8u) >> ((blocks * 64u - 1u - offset) * 8u));
            }
            words[i / 4] |= static_cast<uint32_t>(byte) << (24u - (i % 4) * 8u);
        }
        for (size_t i = 16; i < 80; ++i) {
            words[i] = Rotate(words[i - 3] ^ words[i - 8] ^
                                  words[i - 14] ^ words[i - 16], 1);
        }
        uint32_t a = hash[0], b = hash[1], c = hash[2], d = hash[3], e = hash[4];
        for (size_t i = 0; i < 80; ++i) {
            const uint32_t f = i < 20 ? ((b & c) | (~b & d)) :
                               i < 40 ? (b ^ c ^ d) :
                               i < 60 ? ((b & c) | (b & d) | (c & d)) :
                                        (b ^ c ^ d);
            const uint32_t k = i < 20 ? 0x5a827999u : i < 40 ? 0x6ed9eba1u :
                               i < 60 ? 0x8f1bbcdcu : 0xca62c1d6u;
            const uint32_t next = Rotate(a, 5) + f + e + k + words[i];
            e = d;
            d = c;
            c = Rotate(b, 30);
            b = a;
            a = next;
        }
        hash[0] += a;
        hash[1] += b;
        hash[2] += c;
        hash[3] += d;
        hash[4] += e;
    }
    Sha1Digest digest = {};
    for (size_t i = 0; i < digest.size(); ++i) {
        digest[i] = static_cast<uint8_t>(hash[i / 4] >> (24u - (i % 4) * 8u));
    }
    return digest;
}

std::string Sha1Hex(const std::vector<uint8_t>& bytes) {
    constexpr char hex[] = "0123456789abcdef";
    const Sha1Digest digest = Sha1(bytes.data(), bytes.size());
    std::string text;
    for (uint8_t byte : digest) {
        text += hex[byte >> 4];
        text += hex[byte & 15u];
    }
    return text;
}
