#ifndef HMAWL_SHA1_H
#define HMAWL_SHA1_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

using Sha1Digest = std::array<uint8_t, 20>;

Sha1Digest Sha1(const uint8_t* bytes, size_t size);
std::string Sha1Hex(const std::vector<uint8_t>& bytes);

#endif
