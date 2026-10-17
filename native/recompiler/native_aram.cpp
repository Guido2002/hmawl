#include "native_aram.h"
#include "ppc_state.h"

namespace {

void Transfer(PpcState& state, uint32_t count) {
    auto& dsp = *state.dspHardware;
    const bool compiledTask = dsp.task && dsp.task->mode == NativeDspTaskMode::Running && state.executeDspTask;
    if (!state.dspControl || ((*state.dspControl & 4u) == 0 && !compiledTask) ||
        (count & ~0x83ffffe0u) != 0 || (count & 0x03ffffe0u) == 0) {
        throw std::runtime_error("ARAM DMA requires a halted or compiled DSP and a nonzero aligned transfer length.");
    }
    const auto length = count & 0x03ffffe0u;
    const auto offset = ppc_memory_offset(state, dsp.dmaMainAddress, length);
    const uint64_t end = uint64_t(dsp.dmaAramAddress) + length;
    const bool absent = dsp.disconnectedAramExpansion && dsp.aram.size() == 0x1000000u &&
                        dsp.dmaAramAddress >= 0x1000000u && end <= 0x4000000u;
    if (end > dsp.aram.size() && !absent) {
        throw std::runtime_error("ARAM DMA range exceeds configured native ARAM.");
    }
    const bool read = (count & 0x80000000u) != 0;
    if (absent) {
        // A disconnected HSP has no storage and drives no incoming DMA data.
        if (read) ++dsp.absentAramReads;
        else ++dsp.absentAramWrites;
    } else {
        for (uint32_t index = 0; index < length; ++index) {
            if (read) state.memory[offset + index] = dsp.aram[dsp.dmaAramAddress + index];
            else dsp.aram[dsp.dmaAramAddress + index] = state.memory[offset + index];
        }
    }
    dsp.dmaMainAddress = (dsp.dmaMainAddress + length) & 0x03ffffffu;
    dsp.dmaAramAddress = (dsp.dmaAramAddress + length) & 0x03ffffffu;
    dsp.dmaCount = count & 0x80000000u;
    ++dsp.dmaTransfers;
    *state.dspControl |= 0x20u;
}

const uint32_t& Register(const PpcState& state, uint32_t address) {
    const auto& dsp = *state.dspHardware;
    switch (address & ~3u) {
    case 0xcc005020u: return dsp.dmaMainAddress;
    case 0xcc005024u: return dsp.dmaAramAddress;
    case 0xcc005028u: return dsp.dmaCount;
    default: throw std::runtime_error("Invalid native ARAM DMA register.");
    }
}

} // namespace

uint32_t NativeAramRead(const PpcState& state, uint32_t address, unsigned width) {
    if (!state.dspHardware || (width != 2 && width != 4) || address % width != 0) {
        throw std::runtime_error("Invalid native ARAM register read.");
    }
    const auto& dsp = *state.dspHardware;
    if (width == 2) {
        if (address == 0xcc005012u) return dsp.aramConfiguration;
        if (address == 0xcc005016u) {
            if (!dsp.aramControllerReady) throw std::runtime_error("Native ARAM controller is not initialized.");
            return 1;
        }
        if (address == 0xcc00501au) return dsp.aramRefresh;
    }
    const auto value = Register(state, address);
    return width == 4 ? value : (address & 2u) ? value & 0xffffu : value >> 16;
}

void NativeAramWrite(PpcState& state, uint32_t address, uint32_t value, unsigned width) {
    if (!state.dspHardware || (width != 2 && width != 4) || address % width != 0) {
        throw std::runtime_error("Invalid native ARAM register write.");
    }
    auto& dsp = *state.dspHardware;
    if (width == 2 && address == 0xcc005012u) {
        if ((value & ~0x60u) != 3u || value > 0x7fu) {
            throw std::runtime_error("Unsupported native ARAM configuration.");
        }
        dsp.aramConfiguration = static_cast<uint16_t>(value);
        return;
    }
    if (width == 2 && address == 0xcc00501au) {
        if (value > 0x7ffu) throw std::runtime_error("Unsupported ARAM refresh reserved bits.");
        dsp.aramRefresh = static_cast<uint16_t>(value);
        return;
    }
    if (address == 0xcc005016u) throw std::runtime_error("ARAM controller mode is read-only.");
    auto combined = Register(state, address);
    if (width == 2) {
        if (address & 2u) combined = (combined & 0xffff0000u) | (value & 0xffe0u);
        else combined = (combined & 0xffffu) |
             ((value & ((address & ~3u) == 0xcc005028u ? 0x83ffu : 0x3ffu)) << 16);
    } else {
        combined = value;
    }
    const auto base = address & ~3u;
    if (base == 0xcc005028u) {
        if (width == 4 || (address & 2u)) Transfer(state, combined);
        else dsp.dmaCount = combined;
    } else {
        if (combined & ~0x03ffffe0u) {
            throw std::runtime_error("ARAM DMA address must be 32-byte aligned and fit its register.");
        }
        (base == 0xcc005020u ? dsp.dmaMainAddress : dsp.dmaAramAddress) = combined;
    }
}
