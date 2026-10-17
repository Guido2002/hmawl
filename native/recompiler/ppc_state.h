#ifndef HMAWL_PPC_STATE_H
#define HMAWL_PPC_STATE_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <vector>

constexpr uint32_t kGameCubeRamSize = 24u * 1024u * 1024u;

enum class PpcMemoryMap { Flat, GameCubeRam };

struct PpcState {
    uint32_t gpr[32] = {};
    uint32_t cr = 0;
    uint32_t xer = 0;
    uint32_t lr = 0;
    uint32_t ctr = 0;
    uint32_t pc = 0;
    std::vector<uint8_t> memory;
    PpcMemoryMap memoryMap = PpcMemoryMap::Flat;
    std::function<void(uint32_t, PpcState&)> callGuest;
    std::function<uint64_t()> readTimeBase;
    std::function<void(PpcState&)> submitDvdRead;
    std::function<void(PpcState&)> cancelDvdRead;
    bool externalInterruptsEnabled = true;
};

inline size_t ppc_memory_offset(const PpcState& state, uint32_t address,
                                size_t size) {
    uint32_t offset = address;
    if (state.memoryMap == PpcMemoryMap::GameCubeRam) {
        if (address >= 0x80000000u && address < 0x80000000u + kGameCubeRamSize) {
            offset -= 0x80000000u;
        } else if (address >= 0xc0000000u && address < 0xc0000000u + kGameCubeRamSize) {
            offset -= 0xc0000000u;
        } else if (address >= kGameCubeRamSize) {
            throw std::out_of_range("PowerPC address is not mapped to GameCube RAM");
        }
        if (size > kGameCubeRamSize - offset) {
            throw std::out_of_range("PowerPC access crosses the GameCube RAM boundary");
        }
    }
    if (offset > state.memory.size() || size > state.memory.size() - offset) {
        throw std::out_of_range("PowerPC access is outside guest memory");
    }
    return offset;
}

inline uint32_t ppc_read_u32(const PpcState& state, uint32_t address) {
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint32_t));
    return (static_cast<uint32_t>(state.memory[offset]) << 24) |
           (static_cast<uint32_t>(state.memory[offset + 1]) << 16) |
           (static_cast<uint32_t>(state.memory[offset + 2]) << 8) |
           static_cast<uint32_t>(state.memory[offset + 3]);
}

inline uint16_t ppc_read_u16(const PpcState& state, uint32_t address) {
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint16_t));
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(state.memory[offset]) << 8) |
        static_cast<uint16_t>(state.memory[offset + 1]));
}

inline void ppc_write_u32(PpcState& state, uint32_t address, uint32_t value) {
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint32_t));
    state.memory[offset] = static_cast<uint8_t>(value >> 24);
    state.memory[offset + 1] = static_cast<uint8_t>(value >> 16);
    state.memory[offset + 2] = static_cast<uint8_t>(value >> 8);
    state.memory[offset + 3] = static_cast<uint8_t>(value);
}

inline void ppc_write_u16(PpcState& state, uint32_t address, uint16_t value) {
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint16_t));
    state.memory[offset] = static_cast<uint8_t>(value >> 8);
    state.memory[offset + 1] = static_cast<uint8_t>(value);
}

inline uint8_t ppc_read_u8(const PpcState& state, uint32_t address) {
    return state.memory[ppc_memory_offset(state, address, 1)];
}

inline void ppc_write_u8(PpcState& state, uint32_t address, uint8_t value) {
    state.memory[ppc_memory_offset(state, address, 1)] = value;
}

inline uint32_t ppc_add_carry(PpcState& state, uint32_t left, uint32_t right) {
    const uint64_t sum = static_cast<uint64_t>(left) + right;
    state.xer = (state.xer & ~0x20000000u) |
                (sum > UINT32_MAX ? 0x20000000u : 0u);
    return static_cast<uint32_t>(sum);
}

inline uint32_t ppc_subtract_carry(PpcState& state, uint32_t left, uint32_t right) {
    state.xer = (state.xer & ~0x20000000u) | (right >= left ? 0x20000000u : 0u);
    return right - left;
}

inline uint32_t ppc_subtract_extended(PpcState& state, uint32_t left, uint32_t right) {
    const uint64_t sum = (uint64_t{left} ^ uint64_t{UINT32_MAX}) +
                         right + ((state.xer >> 29) & 1u);
    state.xer = (state.xer & ~0x20000000u) |
                (sum > UINT32_MAX ? 0x20000000u : 0u);
    return static_cast<uint32_t>(sum);
}

inline uint32_t ppc_divide_unsigned32(uint32_t dividend, uint32_t divisor) {
    if (divisor == 0) {
        throw std::runtime_error("Undefined PowerPC divwu result: zero divisor");
    }
    return dividend / divisor;
}

inline uint32_t ppc_rotate_left32(uint32_t value, uint32_t shift) {
    shift &= 31u;
    if (shift == 0) {
        return value;
    }
    return (value << shift) | (value >> (32u - shift));
}

inline uint32_t ppc_count_leading_zeros32(uint32_t value) {
    uint32_t count = 0;
    for (uint32_t mask = 0x80000000u; mask != 0 && (value & mask) == 0; mask >>= 1) {
        ++count;
    }
    return count;
}

inline uint32_t ppc_mask32(uint32_t begin, uint32_t end) {
    uint32_t mask = 0;
    begin &= 31u;
    end &= 31u;
    for (uint32_t bit = begin;; bit = (bit + 1u) & 31u) {
        mask |= 1u << (31u - bit);
        if (bit == end) {
            return mask;
        }
    }
}

inline uint32_t ppc_sign_extend16(uint16_t value) {
    return (value & 0x8000u) != 0 ? 0xffff0000u | value : value;
}

inline void ppc_set_cr_field(PpcState& state, uint32_t field, bool less,
                             bool greater, bool equal) {
    const uint32_t shift = 28u - field * 4u;
    const uint32_t summaryOverflow = (state.xer >> 31) & 1u;
    const uint32_t value = (static_cast<uint32_t>(less) << 3) |
                           (static_cast<uint32_t>(greater) << 2) |
                           (static_cast<uint32_t>(equal) << 1) |
                           summaryOverflow;
    state.cr = (state.cr & ~(0xfu << shift)) | (value << shift);
}

inline bool ppc_branch_condition(PpcState& state, uint32_t bo, uint32_t bi) {
    const bool ctrCondition = (bo & 0x04u) != 0 ||
                              (((--state.ctr != 0) ? 1u : 0u) !=
                               ((bo >> 1) & 1u));
    const bool crCondition = (bo & 0x10u) != 0 ||
                             (((state.cr >> (31u - bi)) & 1u) ==
                              ((bo >> 3) & 1u));
    return ctrCondition && crCondition;
}

inline void ppc_call_guest(PpcState& state, uint32_t address) {
    if (!state.callGuest) {
        throw std::runtime_error("No native translation registered for guest call");
    }
    state.callGuest(address, state);
}

inline uint64_t ppc_read_time_base(const PpcState& state) {
    if (!state.readTimeBase) {
        throw std::runtime_error("No native PowerPC time-base source configured");
    }
    return state.readTimeBase();
}

#endif
