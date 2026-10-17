#ifndef HMAWL_PPC_STATE_H
#define HMAWL_PPC_STATE_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

constexpr uint32_t kGameCubeRamSize = 24u * 1024u * 1024u;

enum class PpcMemoryMap { Flat, GameCubeRam };

struct ExiImmediateRequest {
    unsigned channel;
    unsigned device;
    unsigned length;
    unsigned direction; // 0 read, 1 write, 2 read/write.
    uint32_t data;
};

struct ExiDmaRequest {
    unsigned channel;
    unsigned device;
    unsigned direction;
    unsigned length;
    std::vector<uint8_t> output;
};

struct SiTransferRequest {
    unsigned channel = 0;
    std::vector<uint8_t> output;
    unsigned inputLength = 0;
};

struct SiTransferResult {
    std::vector<uint8_t> input;
    uint32_t error = 0;
};

struct PpcSerialInterface {
    uint32_t polling = 0;
    uint32_t communication = 0;
    uint32_t status = 0;
    std::array<uint32_t, 32> buffer = {};
};

struct PpcDspHardware {
    uint16_t aramConfiguration = 0;
    uint32_t dmaMainAddress = 0;
    uint32_t dmaAramAddress = 0;
    uint32_t dmaCount = 0;
    uint32_t toDspMailbox = 0;
    uint32_t fromDspMailbox = 0;
    uint32_t resetCount = 0;
    uint32_t dmaTransfers = 0;
    std::array<uint16_t, 4096> dataRam = {};
    uint16_t instructionPc = 0;
    bool programHalted = true;
    uint64_t executedInstructions = 0;
    uint32_t completedPrograms = 0;
    uint32_t sentMail = 0;
    uint32_t consumedMail = 0;
    std::vector<uint8_t> aram;
};

struct PpcFpr {
    uint64_t ps0 = 0;
    uint64_t ps1 = 0;

    bool operator==(const PpcFpr& other) const {
        return ps0 == other.ps0 && ps1 == other.ps1;
    }
};

struct PpcState {
    uint32_t gpr[32] = {};
    // Raw IEEE-754 bits keep NaN payloads and the independent paired lane intact.
    std::array<PpcFpr, 32> fpr = {};
    uint32_t fpscr = 0;
    std::array<uint32_t, 8> gqr = {};
    uint32_t msr = 0; // EE is owned by externalInterruptsEnabled.
    uint32_t hid2 = 0;
    uint32_t hid0 = 0;
    uint32_t l2cr = 0;
    std::array<uint32_t, 6> performanceMonitor = {}; // MMCR0/1, PMC1..4; reset only.
    uint32_t cr = 0;
    uint32_t xer = 0;
    uint32_t lr = 0;
    uint32_t ctr = 0;
    uint32_t pc = 0;
    uint32_t srr0 = 0;
    uint32_t srr1 = 0;
    bool systemCallActive = false;
    std::vector<uint8_t> memory;
    PpcMemoryMap memoryMap = PpcMemoryMap::Flat;
    std::function<void(uint32_t, PpcState&)> callGuest;
    std::function<void(uint32_t, uint32_t, PpcState&)> callException;
    std::function<uint64_t()> readTimeBase;
    std::function<void(PpcState&)> submitDvdRead;
    std::function<void(PpcState&)> cancelDvdRead;
    // Device state is opt-in and is not restored with DVD callback CPU context.
    std::optional<uint32_t> piInterruptMask;
    std::optional<uint16_t> miInterruptMask;
    std::optional<uint16_t> dspControl;
    std::optional<PpcDspHardware> dspHardware;
    std::function<void(PpcState&)> executeDspInitialization;
    std::optional<uint32_t> aiControl;
    std::optional<uint32_t> diConfiguration;
    std::optional<uint16_t> viDtvStatus;
    std::optional<PpcSerialInterface> serialInterface;
    std::function<SiTransferResult(const SiTransferRequest&)> transferSi;
    std::array<std::optional<uint32_t>, 3> exiStatus;
    std::array<std::optional<uint32_t>, 3> exiTransferControl;
    bool exiInitializationEnabled = false;
    bool exiImmediateEnabled = false;
    std::array<uint32_t, 3> exiImmediateData = {};
    std::function<uint32_t(const ExiImmediateRequest&)> transferExiImmediate;
    bool exiDmaEnabled = false;
    std::array<uint32_t, 3> exiDmaAddress = {};
    std::array<uint32_t, 3> exiDmaLength = {};
    std::function<std::vector<uint8_t>(const ExiDmaRequest&)> transferExiDma;
    std::function<void(unsigned, uint32_t)> selectExi;
    std::function<void(const std::string&)> writeConsole;
    bool externalInterruptsEnabled = true;
};

inline uint32_t ppc_read_msr(const PpcState& state) {
    return (state.msr & ~0x8000u) | (state.externalInterruptsEnabled ? 0x8000u : 0u);
}

inline uint32_t ppc_shift_right_arithmetic(PpcState& state, uint32_t value, uint32_t shift) {
    shift &= 63u;
    const bool negative = (value & 0x80000000u) != 0;
    const bool carry = negative && shift != 0 &&
        (shift >= 32 || (value & ((1u << shift) - 1u)) != 0);
    state.xer = (state.xer & ~0x20000000u) | (carry ? 0x20000000u : 0u);
    if (shift == 0) return value;
    if (shift >= 32) return negative ? UINT32_MAX : 0;
    return (value >> shift) | (negative ? UINT32_MAX << (32u - shift) : 0);
}

inline void ppc_reset_performance_monitor(PpcState& state, unsigned index, uint32_t value) {
    if (index >= state.performanceMonitor.size() || value != 0) {
        throw std::runtime_error("Unsupported PowerPC performance-monitor write (only zero resets supported).");
    }
    state.performanceMonitor[index] = 0;
}

inline void ppc_check_pending_interrupts(const PpcState& state, bool enabled) {
    if (enabled && state.piInterruptMask.has_value() && (*state.piInterruptMask & 8u) != 0 &&
        state.serialInterface.has_value() &&
        (state.serialInterface->communication & 0xc0000000u) == 0xc0000000u) {
        throw std::runtime_error("Native SI completion interrupt requires guest interrupt delivery.");
    }
    if (enabled && state.piInterruptMask.has_value() && (*state.piInterruptMask & 0x80u) != 0 &&
        state.dspHardware.has_value() && state.dspControl.has_value()) {
        const auto control = *state.dspControl;
        if ((control & 0x180u) == 0x180u || (control & 0x60u) == 0x60u ||
            (control & 0x18u) == 0x18u) {
            throw std::runtime_error("Native DSP interrupt requires guest interrupt delivery.");
        }
    }
}

inline void ppc_write_msr(PpcState& state, uint32_t value) {
    if (((ppc_read_msr(state) ^ value) & ~0xa002u) != 0) {
        throw std::runtime_error("Unsupported PowerPC MSR mode change (only FP/EE/RI supported).");
    }
    ppc_check_pending_interrupts(state, (value & 0x8000u) != 0);
    state.msr = value & ~0x8000u;
    state.externalInterruptsEnabled = (value & 0x8000u) != 0;
}

inline void ppc_write_hid2(PpcState& state, uint32_t value) {
    if (((state.hid2 ^ value) & ~0xa0000000u) != 0) {
        throw std::runtime_error("Unsupported PowerPC HID2 change (only PSE/LSQE supported).");
    }
    state.hid2 = value;
}

inline void ppc_sync() {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

inline void ppc_write_hid0(PpcState& state, uint32_t value) {
    if (((state.hid0 ^ value) & ~0xca08u) != 0) {
        throw std::runtime_error("Unsupported PowerPC HID0 change (only ICE/DCE/ICFI/SPD/DCFA supported).");
    }
    // RAM is coherent and translations are static, with code checks at dispatch.
    if ((value & 0x800u) != 0) {
        ppc_sync();
    }
    state.hid0 = value & ~0x800u;
}

inline void ppc_write_l2cr(PpcState& state, uint32_t value) {
    if (((state.l2cr ^ value) & ~0x80200000u) != 0) {
        throw std::runtime_error("Unsupported PowerPC L2CR change (only L2E/L2I supported).");
    }
    if ((value & 0x80200000u) == 0x80200000u) {
        throw std::runtime_error("PowerPC L2 invalidation requires the L2 cache to be disabled.");
    }
    // Invalidation completes synchronously over coherent RAM; L2IP stays clear.
    if ((value & 0x00200000u) != 0) {
        ppc_sync();
    }
    state.l2cr = value;
}

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

inline void ppc_flush_cache_block(const PpcState& state, uint32_t address) {
    ppc_memory_offset(state, address & ~31u, 32);
    // Both caches share coherent RAM; static code checks remain authoritative.
    ppc_sync();
}

inline uint32_t ppc_read_u32(const PpcState& state, uint32_t address) {
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.dspHardware.has_value()) {
        const auto& dsp = *state.dspHardware;
        if (address == 0xcc005020u) return dsp.dmaMainAddress;
        if (address == 0xcc005024u) return dsp.dmaAramAddress;
        if (address == 0xcc005028u) return dsp.dmaCount;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.exiDmaEnabled) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            if (address == 0xcc006804u + channel * 0x14u) return state.exiDmaAddress[channel];
            if (address == 0xcc006808u + channel * 0x14u) return state.exiDmaLength[channel];
        }
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.serialInterface.has_value()) {
        const auto& serial = *state.serialInterface;
        if (address == 0xcc006430u) return serial.polling;
        if (address == 0xcc006434u) {
            if ((serial.communication & 1u) != 0) {
                throw std::runtime_error("Native SI transfer pending without a completion backend.");
            }
            return serial.communication;
        }
        if (address == 0xcc006438u) return serial.status;
        if (address >= 0xcc006480u && address <= 0xcc0064fcu && (address & 3u) == 0) {
            return serial.buffer[(address - 0xcc006480u) / 4u];
        }
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc006024u &&
        state.diConfiguration.has_value()) {
        return *state.diConfiguration;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.exiImmediateEnabled &&
        (address == 0xcc006810u || address == 0xcc006824u || address == 0xcc006838u)) {
        return state.exiImmediateData[(address - 0xcc006810u) / 0x14u];
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam &&
        (address == 0xcc00680cu || address == 0xcc006820u || address == 0xcc006834u)) {
        const auto& control = state.exiTransferControl[(address - 0xcc00680cu) / 0x14u];
        if (control.has_value()) {
            if ((*control & ~0x3eu) != 0) {
                throw std::runtime_error("Unsupported EXI transfer control read (only idle state supported).");
            }
            return *control;
        }
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam &&
        (address == 0xcc006800u || address == 0xcc006814u || address == 0xcc006828u)) {
        const auto& status = state.exiStatus[(address - 0xcc006800u) / 0x14u];
        if (status.has_value()) return *status;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc006c00u &&
        state.aiControl.has_value()) {
        return *state.aiControl;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc003004u &&
        state.piInterruptMask.has_value()) {
        return *state.piInterruptMask;
    }
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint32_t));
    return (static_cast<uint32_t>(state.memory[offset]) << 24) |
           (static_cast<uint32_t>(state.memory[offset + 1]) << 16) |
           (static_cast<uint32_t>(state.memory[offset + 2]) << 8) |
           static_cast<uint32_t>(state.memory[offset + 3]);
}

inline uint64_t ppc_read_u64(const PpcState& state, uint32_t address) {
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint64_t));
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) {
        value = (value << 8) | state.memory[offset + i];
    }
    return value;
}

inline uint64_t ppc_widen_single_bits(uint32_t bits) {
    const uint64_t sign = static_cast<uint64_t>(bits & 0x80000000u) << 32;
    const uint32_t exponent = (bits >> 23) & 0xffu;
    uint32_t fraction = bits & 0x7fffffu;
    if (exponent != 0) {
        const uint64_t widenedExponent = exponent == 255 ? 2047u : exponent + 896u;
        return sign | (widenedExponent << 52) | (static_cast<uint64_t>(fraction) << 29);
    }
    if (fraction == 0) {
        return sign;
    }
    unsigned shift = 0;
    while ((fraction & 0x800000u) == 0) {
        fraction <<= 1;
        ++shift;
    }
    return sign | (static_cast<uint64_t>(897u - shift) << 52) |
           (static_cast<uint64_t>(fraction & 0x7fffffu) << 29);
}

inline void ppc_write_u64(PpcState& state, uint32_t address, uint64_t value) {
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint64_t));
    for (unsigned i = 0; i < 8; ++i) {
        state.memory[offset + i] = static_cast<uint8_t>(value >> (56u - i * 8u));
    }
}

inline uint16_t ppc_read_u16(const PpcState& state, uint32_t address) {
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.dspHardware.has_value()) {
        const auto& dsp = *state.dspHardware;
        if (address == 0xcc005000u) return static_cast<uint16_t>(dsp.toDspMailbox >> 16);
        if (address == 0xcc005002u) return static_cast<uint16_t>(dsp.toDspMailbox);
        if (address == 0xcc005004u) return static_cast<uint16_t>(dsp.fromDspMailbox >> 16);
        if (address == 0xcc005006u) {
            if ((dsp.fromDspMailbox & 0x80000000u) != 0) {
                throw std::runtime_error("DSP mailbox consumption requires a native DSP execution backend.");
            }

            return static_cast<uint16_t>(dsp.fromDspMailbox);
        }
        if (address == 0xcc005012u) return dsp.aramConfiguration;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc00206cu &&
        state.viDtvStatus.has_value()) {
        return *state.viDtvStatus;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc00500au &&
        state.dspControl.has_value()) {
        return *state.dspControl;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc00401cu &&
        state.miInterruptMask.has_value()) {
        return *state.miInterruptMask;
    }
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint16_t));
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(state.memory[offset]) << 8) |
        static_cast<uint16_t>(state.memory[offset + 1]));
}

inline uint16_t ppc_read_u16(PpcState& state, uint32_t address) {
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc005006u &&
        state.dspHardware.has_value() && state.executeDspInitialization) {
        auto& dsp = *state.dspHardware;
        const auto value = static_cast<uint16_t>(dsp.fromDspMailbox);
        if ((dsp.fromDspMailbox & 0x80000000u) != 0) {
            dsp.fromDspMailbox &= ~0x80000000u;
            ++dsp.consumedMail;
        }
        return value;
    }
    return ppc_read_u16(static_cast<const PpcState&>(state), address);
}

inline void ppc_write_u32(PpcState& state, uint32_t address, uint32_t value) {
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.dspHardware.has_value()) {
        auto& dsp = *state.dspHardware;
        if (address == 0xcc005020u || address == 0xcc005024u) {
            if ((value & ~0x03ffffe0u) != 0) {
                throw std::runtime_error("ARAM DMA address must be 32-byte aligned and fit its register.");
            }
            (address == 0xcc005020u ? dsp.dmaMainAddress : dsp.dmaAramAddress) = value;
            return;
        }
        if (address == 0xcc005028u) {
            if (!state.dspControl.has_value() || (*state.dspControl & 4u) == 0 ||
                (value & ~0x83ffffe0u) != 0 || (value & 0x03ffffe0u) == 0) {
                throw std::runtime_error("ARAM DMA requires halted DSP and a nonzero aligned transfer length.");
            }
            const auto length = value & 0x03ffffe0u;
            const auto offset = ppc_memory_offset(state, dsp.dmaMainAddress, length);
            if (dsp.dmaAramAddress > dsp.aram.size() ||
                length > dsp.aram.size() - dsp.dmaAramAddress) {
                throw std::runtime_error("ARAM DMA range exceeds configured native ARAM.");
            }
            const bool read = (value & 0x80000000u) != 0;
            for (size_t index = 0; index < length; ++index) {
                if (read) state.memory[offset + index] = dsp.aram[dsp.dmaAramAddress + index];
                else dsp.aram[dsp.dmaAramAddress + index] = state.memory[offset + index];
            }
            dsp.dmaCount = value;
            ++dsp.dmaTransfers;
            *state.dspControl |= 0x20u;
            return;
        }
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.exiDmaEnabled) {
        for (unsigned channel = 0; channel < 3; ++channel) {
            const auto base = 0xcc006800u + channel * 0x14u;
            if (address != base + 4 && address != base + 8 &&
                !(address == base + 12 && (value & 2u) != 0)) continue;
            auto& control = state.exiTransferControl[channel];
            auto& status = state.exiStatus[channel];
            if (!control.has_value() || !status.has_value() || (*control & ~0x3eu) != 0) {
                throw std::runtime_error("EXI DMA requires mapped idle control and status.");
            }
            if (address == base + 4 || address == base + 8) {
                if ((value & ~0x03ffffe0u) != 0) {
                    throw std::runtime_error("EXI DMA address/length must be aligned and fit its register.");
                }
                (address == base + 4 ? state.exiDmaAddress[channel] : state.exiDmaLength[channel]) = value;
                return;
            }
            const auto selected = *status & 0x380u;
            if (value != 3u && value != 7u) {
                throw std::runtime_error("Unsupported EXI DMA transfer mode.");
            }
            if (selected != 0x80u && selected != 0x100u && selected != 0x200u) {
                throw std::runtime_error("EXI DMA requires exactly one selected device.");
            }
            if (!state.transferExiDma) {
                throw std::runtime_error("EXI DMA requires a native peripheral backend.");
            }
            const auto length = state.exiDmaLength[channel];
            if (length == 0) throw std::runtime_error("EXI DMA requires a nonzero length.");
            const auto offset = ppc_memory_offset(state, state.exiDmaAddress[channel], length);
            ExiDmaRequest request{channel, selected == 0x80u ? 0u : selected == 0x100u ? 1u : 2u,
                                  (value >> 2) & 1u, length, {}};
            if (request.direction == 1) {
                request.output.assign(state.memory.begin() + offset, state.memory.begin() + offset + length);
            }
            const auto input = state.transferExiDma(request);
            if (input.size() != (request.direction == 0 ? length : 0u)) {
                throw std::runtime_error("Invalid native EXI DMA result length.");
            }
            for (size_t index = 0; index < input.size(); ++index) state.memory[offset + index] = input[index];
            *control = value & ~1u;
            *status |= 8u;
            return;
        }
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.serialInterface.has_value()) {
        auto& serial = *state.serialInterface;
        if (address == 0xcc006430u) {
            if ((value & ~0x03ffff0fu) != 0) {
                throw std::runtime_error("Unsupported SI polling write (automatic polling is not implemented).");
            }
            serial.polling = value;
            return;
        }
        if (address == 0xcc006434u) {
            if ((serial.communication & 1u) != 0) {
                throw std::runtime_error("Cannot change SI transfer parameters while a transfer is pending.");
            }
            if ((value & ~0xc87f7f07u) != 0) {
                throw std::runtime_error("Unsupported SI communication control write.");
            }
            if ((value & 1u) != 0) {
                if (!state.transferSi) {
                    throw std::runtime_error("Native SI transfer requires a controller transaction/completion backend.");
                }
                SiTransferRequest request;
                request.channel = (value >> 1) & 3u;
                const auto outputLength = ((value >> 16) & 0x7fu) == 0 ?
                    128u : (value >> 16) & 0x7fu;
                request.inputLength = ((value >> 8) & 0x7fu) == 0 ?
                    128u : (value >> 8) & 0x7fu;
                request.output.reserve(outputLength);
                for (unsigned index = 0; index < outputLength; ++index) {
                    request.output.push_back(static_cast<uint8_t>(
                        serial.buffer[index / 4] >> (24u - (index % 4) * 8u)));
                }
                const auto result = state.transferSi(request);
                if ((result.error & ~0xfu) != 0 ||
                    (result.error == 0 ? result.input.size() != request.inputLength : !result.input.empty())) {
                    throw std::runtime_error("Invalid native SI transfer result length or error flags.");
                }
                for (unsigned index = 0; index < result.input.size(); ++index) {
                    const auto shift = 24u - (index % 4) * 8u;
                    auto& word = serial.buffer[index / 4];
                    word = (word & ~(0xffu << shift)) | (uint32_t{result.input[index]} << shift);
                }
                serial.status |= result.error << ((3u - request.channel) * 8u);
                serial.communication = (serial.communication & 0x10000000u) |
                    (value & 0x487f7f06u) | 0x80000000u | (result.error != 0 ? 0x20000000u : 0u);
                return;
            }
            serial.communication = (serial.communication & 0xb0000000u & ~(value & 0x80000000u)) |
                (value & 0x487f7f06u);
            return;
        }
        if (address == 0xcc006438u) {
            if ((value & ~0x0f0f0f0fu) != 0) {
                throw std::runtime_error("Unsupported SI status write (output polling commands are not implemented).");
            }
            serial.status &= ~value;
            return;
        }
        if (address >= 0xcc006480u && address <= 0xcc0064fcu && (address & 3u) == 0) {
            if ((serial.communication & 1u) != 0) {
                throw std::runtime_error("Cannot write SI communication RAM during a pending transfer.");
            }
            serial.buffer[(address - 0xcc006480u) / 4u] = value;
            return;
        }
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.exiImmediateEnabled &&
        (address == 0xcc006810u || address == 0xcc006824u || address == 0xcc006838u)) {
        state.exiImmediateData[(address - 0xcc006810u) / 0x14u] = value;
        return;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.exiImmediateEnabled &&
        (address == 0xcc00680cu || address == 0xcc006820u || address == 0xcc006834u)) {
        const unsigned channel = (address - 0xcc00680cu) / 0x14u;
        auto& control = state.exiTransferControl[channel];
        auto& status = state.exiStatus[channel];
        const uint32_t selected = status.has_value() ? *status & 0x380u : 0;
        if (!control.has_value() || (*control & ~0x3eu) != 0 ||
            !status.has_value() || selected == 0 || (selected & (selected - 1u)) != 0 ||
            (value & ~0x3du) != 0 || (value & 1u) == 0 || (value & 0xcu) == 0xcu) {
            throw std::runtime_error("Unsupported EXI immediate request (requires idle control, one selected device, valid immediate direction and start).");
        }
        if (!state.transferExiImmediate) {
            throw std::runtime_error("EXI immediate transfer requires a native peripheral backend; no response or completion was fabricated.");
        }
        const unsigned device = selected == 0x80u ? 0 : selected == 0x100u ? 1 : 2;
        const uint32_t result = state.transferExiImmediate({
            channel, device, ((value >> 4) & 3u) + 1u, (value >> 2) & 3u,
            state.exiImmediateData[channel]});
        if (((value >> 2) & 3u) != 1u) {
            state.exiImmediateData[channel] = result;
        }
        *control = value & ~1u;
        *status |= 8u;
        return;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam &&
        (address == 0xcc006800u || address == 0xcc006814u || address == 0xcc006828u)) {
        const unsigned channel = (address - 0xcc006800u) / 0x14u;
        auto& status = state.exiStatus[channel];
        if (status.has_value()) {
            const uint32_t previous = *status;
            if (state.exiImmediateEnabled) {
                const uint32_t writable = channel == 0 ? 0x2fffu :
                    channel == 1 ? 0xfffu : 0x3ffu;
                const uint32_t allowed = writable | (channel < 2 ? 0x1000u : 0);
                const uint32_t selected = value & 0x380u;
                const auto& control = state.exiTransferControl[channel];
                if ((previous & ~allowed) != 0 || (value & ~allowed) != 0 ||
                    (selected != 0 && (selected & (selected - 1u)) != 0) ||
                    (value & 0x70u) > 0x50u || !control.has_value() ||
                    (*control & ~0x3eu) != 0) {
                    throw std::runtime_error("Unsupported EXI bus selection/status write.");
                }
                const uint32_t pending = channel < 2 ? 0x80au : 0xau;
                const uint32_t retained = channel == 0 ? 0x3000u :
                    channel == 1 ? 0x1000u : 0;
                if (state.selectExi && (previous & 0x380u) != selected) {
                    state.selectExi(channel, selected);
                }
                *status = (previous & retained) | (previous & pending & ~value) |
                    (value & writable & ~pending);
                return;
            }
            const uint32_t supported = channel == 2 ? 0x7fu :
                channel == 0 && state.exiInitializationEnabled ? 0x3c7fu : 0x1c7fu;
            if (state.exiInitializationEnabled &&
                (value == 0 || (channel == 0 && value == 0x2000u))) {
                const auto& control = state.exiTransferControl[channel];
                if ((previous & ~supported) != 0 || !control.has_value() ||
                    (*control & ~0x3eu) != 0) {
                    throw std::runtime_error("Unsupported EXI status reset (requires mapped idle transfer control and valid status).");
                }
                *status = (previous & (channel == 2 ? 0xau :
                    channel == 0 ? 0x380au : 0x180au)) | value;
                return;
            }
            const uint32_t written = channel == 2 ? 0xfu : channel == 0 ? 0x2c0fu : 0xc0fu;
            if ((previous & ~supported) != 0 || value != (previous & ~written)) {
                throw std::runtime_error("Unsupported EXI status write (only idle startup interrupt masking supported).");
            }
            // Zero in W1C fields retains pending status; EXT is read-only.
            *status = previous & ~(channel == 2 ? 0x5u : 0x405u);
            return;
        }
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc006c00u &&
        state.aiControl.has_value()) {
        const uint32_t previous = *state.aiControl;
        if ((previous & ~0x5eu) != 0 || value != (previous & ~0x2cu)) {
            throw std::runtime_error("Unsupported AI control write (only idle startup interrupt masking supported).");
        }
        // Writing zero to AIINT leaves pending status set; SCRESET is not issued.
        *state.aiControl = previous & ~0x4u;
        return;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc003004u &&
        state.piInterruptMask.has_value()) {
        if (value != 0xf0u && !(state.serialInterface.has_value() && value == 0xf8u)) {
            throw std::runtime_error("Unsupported PI interrupt mask write (only startup mask 0xf0, or 0xf8 with SI mapped, supported).");
        }
        *state.piInterruptMask = value;
        return;
    }
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint32_t));
    state.memory[offset] = static_cast<uint8_t>(value >> 24);
    state.memory[offset + 1] = static_cast<uint8_t>(value >> 16);
    state.memory[offset + 2] = static_cast<uint8_t>(value >> 8);
    state.memory[offset + 3] = static_cast<uint8_t>(value);
}

inline void ppc_write_u16(PpcState& state, uint32_t address, uint16_t value) {
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && state.dspHardware.has_value()) {
        auto& dsp = *state.dspHardware;
        if (address == 0xcc005012u) {
            if (value != 0x43u) throw std::runtime_error("Unsupported native ARAM configuration.");
            dsp.aramConfiguration = value;
            return;
        }
        if (address == 0xcc005000u) {
            if (value != 0 || (dsp.toDspMailbox & 0x80000000u) != 0) {
                throw std::runtime_error("DSP outgoing mailbox requires a native DSP execution backend.");
            }
            dsp.toDspMailbox &= 0xffffu;
            return;
        }
        if (address == 0xcc005002u) {
            throw std::runtime_error("DSP outgoing mailbox requires a native DSP execution backend.");
        }
        if (address == 0xcc00500au) {
            if (!state.dspControl.has_value() || (value & ~0x0fffu) != 0 || (value & 2u) != 0) {
                throw std::runtime_error("Unsupported native DSP control write.");
            }
            const auto previous = *state.dspControl;
            const bool unhalt = (value & 4u) == 0 && ((previous & 4u) != 0 || (value & 1u) != 0);
            if (unhalt && !state.executeDspInitialization) {
                throw std::runtime_error("DSP unhalt requires a native DSP execution backend; no mailbox acknowledgement was fabricated.");
            }
            if ((value & 1u) != 0 && (value & 0x804u) != 0x804u) {
                throw std::runtime_error("DSP reset requires the DSP halted with bootstrap reset asserted.");
            }
            if ((value & 1u) != 0) {
                dsp.toDspMailbox = 0;
                dsp.fromDspMailbox = 0;
                ++dsp.resetCount;
                dsp.programHalted = true;
            }
            *state.dspControl = static_cast<uint16_t>(
                (previous & 0x02a8u & ~value) | (value & 0x0954u));
            if (unhalt) {
                dsp.programHalted = false;
                state.executeDspInitialization(state);
                if (!dsp.programHalted) {
                    throw std::runtime_error("Native DSP initialization returned without a compiled HALT.");
                }
                *state.dspControl |= 4u;
            }
            return;
        }
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc00500au &&
        state.dspControl.has_value()) {
        const uint16_t previous = *state.dspControl;
        if ((previous & 0xf803u) != 0 || value != (previous & ~0x01f8u)) {
            throw std::runtime_error("Unsupported DSP control write (only startup interrupt masking supported).");
        }
        // Status bits 0xa8 are write-one-to-clear: the startup's zero writes retain them.
        *state.dspControl = static_cast<uint16_t>(previous & ~0x0150u);
        return;
    }
    if (state.memoryMap == PpcMemoryMap::GameCubeRam && address == 0xcc00401cu &&
        state.miInterruptMask.has_value()) {
        if (value != 0) {
            throw std::runtime_error("Unsupported MI interrupt mask write (only zero reset supported).");
        }
        *state.miInterruptMask = value;
        return;
    }
    const size_t offset = ppc_memory_offset(state, address, sizeof(uint16_t));
    state.memory[offset] = static_cast<uint8_t>(value >> 8);
    state.memory[offset + 1] = static_cast<uint8_t>(value);
}

inline uint8_t ppc_read_u8(const PpcState& state, uint32_t address) {
    return state.memory[ppc_memory_offset(state, address, 1)];
}

inline uint64_t ppc_dequantize_integer_bits(uint32_t raw, uint32_t type,
                                           uint32_t scale) {
    const unsigned width = (type & 1u) != 0 ? 16u : 8u;
    const uint32_t mask = (1u << width) - 1u;
    raw &= mask;
    const bool negative = (type & 2u) != 0 && (raw & (1u << (width - 1u))) != 0;
    const uint32_t magnitude = negative ? ((~raw + 1u) & mask) : raw;
    if (magnitude == 0) {
        return 0;
    }
    unsigned leading = 0;
    for (uint32_t remaining = magnitude; remaining > 1; remaining >>= 1) {
        ++leading;
    }
    const int signedScale = scale < 32 ? static_cast<int>(scale) :
                                        static_cast<int>(scale) - 64;
    // All 8/16-bit integers scaled by a GQR power of two fit exactly in both precisions.
    const uint64_t exponent = static_cast<uint64_t>(1023 + static_cast<int>(leading) - signedScale);
    return (negative ? UINT64_C(0x8000000000000000) : 0) | (exponent << 52) |
           (static_cast<uint64_t>(magnitude - (1u << leading)) << (52u - leading));
}

inline PpcFpr ppc_load_quantized_pair(const PpcState& state, uint32_t address,
                                      uint32_t quantization, bool single) {
    const uint32_t gqr = state.gqr.at(quantization);
    const uint32_t type = (gqr >> 16) & 7u;
    if (type >= 1 && type <= 3) {
        throw std::runtime_error("Unsupported PowerPC psq_l quantized load type.");
    }
    const uint32_t width = type == 0 ? 4u : (type & 1u) != 0 ? 2u : 1u;
    ppc_memory_offset(state, address, single ? width : width * 2u);
    const auto load = [&](uint32_t source) {
        if (type == 0) {
            return ppc_widen_single_bits(ppc_read_u32(state, source));
        }
        const uint32_t raw = width == 2 ? ppc_read_u16(state, source) :
                                         ppc_read_u8(state, source);
        return ppc_dequantize_integer_bits(raw, type, (gqr >> 24) & 63u);
    };
    const uint64_t first = load(address);
    const uint64_t second = single ? UINT64_C(0x3ff0000000000000) : load(address + width);
    return {first, second};
}

inline void ppc_write_u8(PpcState& state, uint32_t address, uint8_t value) {
    state.memory[ppc_memory_offset(state, address, 1)] = value;
}

inline uint32_t ppc_add_carry(PpcState& state, uint32_t left, uint32_t right,
                              bool extended = false) {
    const uint64_t sum = static_cast<uint64_t>(left) + right +
                         (extended ? ((state.xer >> 29) & 1u) : 0u);
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

inline void ppc_write_fpscr(PpcState& state, uint32_t value, uint32_t mask) {
    uint32_t result = (state.fpscr & ~mask) | (value & mask);
    // VX and FEX are derived summaries, not independently writable flags.
    constexpr uint32_t invalidCauses = 0x01f80700u;
    result &= ~0x60000000u;
    if ((result & invalidCauses) != 0) {
        result |= 0x20000000u;
    }
    if (((result >> 22) & result & 0xf8u) != 0) {
        result |= 0x40000000u;
    }
    state.fpscr = result;
}

inline void ppc_write_fpscr_bit(PpcState& state, uint32_t mask, bool set) {
    uint32_t value = set ? state.fpscr | mask : state.fpscr & ~mask;
    if (set && (mask & 0x1ff80700u) != 0 && (state.fpscr & mask) == 0) {
        value |= 0x80000000u;
    }
    ppc_write_fpscr(state, value, UINT32_MAX);
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

inline void ppc_system_call(PpcState& state, uint32_t next) {
    if (state.memoryMap != PpcMemoryMap::GameCubeRam || state.systemCallActive ||
        !state.callException || (ppc_read_msr(state) & ~0xa032u) != 0) {
        throw std::runtime_error("Unsupported or nested native PowerPC system-call context.");
    }
    state.srr0 = next;
    state.srr1 = ppc_read_msr(state);
    state.systemCallActive = true;
    state.msr = 0;
    state.externalInterruptsEnabled = false;
    state.callException(0xc00u, next, state);
    if (state.systemCallActive) {
        throw std::runtime_error("Native PowerPC system-call handler returned without rfi.");
    }
}

inline void ppc_return_from_system_call(PpcState& state) {
    if (!state.systemCallActive || (state.srr0 & 3u) != 0 ||
        (state.srr1 & ~0xa032u) != 0) {
        throw std::runtime_error("Native rfi requires a supported active system-call context.");
    }
    ppc_check_pending_interrupts(state, (state.srr1 & 0x8000u) != 0);
    state.msr = state.srr1 & ~0x8000u;
    state.externalInterruptsEnabled = (state.srr1 & 0x8000u) != 0;
    state.pc = state.srr0;
    state.systemCallActive = false;
}

inline uint64_t ppc_read_time_base(const PpcState& state) {
    if (!state.readTimeBase) {
        throw std::runtime_error("No native PowerPC time-base source configured");
    }
    return state.readTimeBase();
}

#endif
