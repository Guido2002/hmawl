#ifndef HMAWL_NATIVE_GX_H
#define HMAWL_NATIVE_GX_H

#include "native_efb.h"

#include <array>
#include <cstdint>
#include <vector>

struct PpcState;

struct NativeGx {
    uint16_t control = 0;
    uint16_t watermarkStatus = 0;
    uint16_t cpToken = 0;
    uint16_t performanceSelect = 0;
    uint64_t performanceBursts = 0;
    uint64_t performanceCommands = 0;
    uint64_t performanceResets = 0;
    std::array<uint32_t, 8> fifo = {};
    uint32_t cpuBase = 0;
    uint32_t cpuEnd = 0;
    uint32_t cpuWrite = 0;
    uint32_t writeGatherAddress = 0;
    std::vector<uint8_t> gathered;
    std::vector<uint8_t> commands;
    std::array<uint32_t, 256> cp = {};
    std::array<uint32_t, 256> bp = {};
    std::vector<uint32_t> xf = std::vector<uint32_t>(0x1058);
    uint32_t bpMask = 0xffffffu;
    uint16_t peControl = 0;
    uint16_t pePending = 0;
    std::array<uint16_t, 5> poke = {};
    uint16_t token = 0;
    uint64_t bursts = 0;
    uint64_t decodedCommands = 0;
    uint64_t vertexCacheInvalidations = 0;
    uint64_t texturePreloads = 0;
    uint64_t texturePreloadBytes = 0;
    uint64_t draws = 0;
    uint64_t rasterizedPixels = 0;
    std::optional<NativeEfb> efb;
};

uint32_t NativeGxInterruptCause(const NativeGx& gx);
uint32_t NativeGxRead(const PpcState& state, uint32_t address, unsigned width);
void NativeGxWrite(PpcState& state, uint32_t address, uint32_t value, unsigned width);
void NativeGxSetWriteGatherAddress(PpcState& state, uint32_t value);

inline bool NativeGxAddress(uint32_t address) {
    return (address >= 0xcc000000u && address < 0xcc000040u) ||
           (address >= 0xcc001000u && address < 0xcc001010u) ||
           (address >= 0xcc00300cu && address < 0xcc003018u) ||
           (address >= 0xcc008000u && address < 0xcc008004u);
}

#endif
