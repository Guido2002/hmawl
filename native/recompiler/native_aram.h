#ifndef HMAWL_NATIVE_ARAM_H
#define HMAWL_NATIVE_ARAM_H

#include <cstdint>

struct PpcState;

inline bool NativeAramAddress(uint32_t address) {
    return address == 0xcc005012u || address == 0xcc005016u || address == 0xcc00501au ||
           (address >= 0xcc005020u && address <= 0xcc00502bu);
}

uint32_t NativeAramRead(const PpcState& state, uint32_t address, unsigned width);
void NativeAramWrite(PpcState& state, uint32_t address, uint32_t value, unsigned width);

#endif
