#ifndef HMAWL_NATIVE_VI_H
#define HMAWL_NATIVE_VI_H

#include <array>
#include <cstdint>

struct PpcState;

struct NativeVi {
    std::array<uint16_t, 60> registers = {};
    uint64_t lastTicks = 0;
    uint64_t elapsedTicks = 0;
    uint64_t fields = 0;
    uint64_t writes = 0;
    uint64_t resets = 0;
    bool clockStarted = false;
};

inline bool NativeViAddress(uint32_t address) {
    return address >= 0xcc002000u && address < 0xcc002078u;
}

uint32_t NativeViRead(const PpcState& state, uint32_t address, unsigned width);
void NativeViWrite(PpcState& state, uint32_t address, uint32_t value, unsigned width);
void NativeViAdvance(NativeVi& vi, uint64_t ticks);
uint32_t NativeViInterruptCause(const NativeVi& vi);

#endif
