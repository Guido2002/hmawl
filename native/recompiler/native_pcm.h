#ifndef HMAWL_NATIVE_PCM_H
#define HMAWL_NATIVE_PCM_H

#include <cstdint>

struct PpcState;

struct NativePcm {
    bool active = false;
    uint16_t remainingBlocks = 0;
    uint32_t sampleRate = 0;
    uint64_t lastTicks = 0;
    uint64_t phase = 0;
    uint64_t submittedFrames = 0;
    uint64_t completedBuffers = 0;
    uint64_t starts = 0;
};

void NativePcmWrite(PpcState& state, uint32_t address, uint16_t value);
void NativePcmAdvance(PpcState& state, uint64_t ticks);

#endif
