#ifndef HMAWL_NATIVE_AI_H
#define HMAWL_NATIVE_AI_H

#include <cstdint>

struct PpcState;

struct NativeAi {
    uint16_t volume = 0;
    uint32_t samples = 0;
    uint32_t interruptTiming = 0;
    uint64_t lastTicks = 0;
    uint64_t phase = 0;
    uint64_t advancedSamples = 0;
    uint64_t writes = 0;
    uint64_t resets = 0;
    bool clockStarted = false;
};

inline bool NativeAiAddress(uint32_t address) {
    return address >= 0xcc006c00u && address < 0xcc006c10u;
}

uint32_t NativeAiRead(const PpcState& state, uint32_t address, unsigned width);
void NativeAiWrite(PpcState& state, uint32_t address, uint32_t value, unsigned width);
void NativeAiAdvance(PpcState& state, uint64_t ticks);
uint32_t NativeAiInterruptCause(const PpcState& state);

#endif
