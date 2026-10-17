#ifndef HMAWL_CLZ_IMAGE_H
#define HMAWL_CLZ_IMAGE_H

#include <cstddef>
#include <cstdint>
#include <vector>

std::vector<uint8_t> DecodeClz(const std::vector<uint8_t>& input, size_t maximumOutputSize);

#endif
