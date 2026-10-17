#include "native_gx.h"
#include "ppc_state.h"

#include <stdexcept>
#include <utility>

namespace {

uint32_t Word(const std::vector<uint8_t>& bytes, size_t offset) {
    return (uint32_t(bytes[offset]) << 24) | (uint32_t(bytes[offset + 1]) << 16) |
           (uint32_t(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

bool Breakpoint(const NativeGx& gx) {
    return (gx.control & 2u) != 0 && gx.fifo[6] == gx.fifo[7];
}

void Decode(NativeGx& gx) {
    auto& bytes = gx.commands;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto opcode = bytes[offset];
        size_t length = 1;
        if (opcode == 0x08) length = 6;
        else if (opcode == 0x61) length = 5;
        else if (opcode == 0x10) {
            if (bytes.size() - offset < 5) break;
            const auto header = Word(bytes, offset + 1);
            const auto address = header & 0xffffu;
            const auto count = (header >> 16) + 1;
            if (address >= gx.xf.size() || count > gx.xf.size() - address) {
                throw std::runtime_error("Native GX XF load exceeds transform register memory.");
            }
            length = 5 + 4 * count;
        } else if (opcode != 0 && opcode != 0x48) {
            throw std::runtime_error(opcode >= 0x80
                ? "Native GX draw command requires a vertex/renderer backend."
                : "Unsupported native GX FIFO opcode.");
        }
        if (bytes.size() - offset < length) break;
        if (opcode == 0x08) {
            gx.cp[bytes[offset + 1]] = Word(bytes, offset + 2);
        } else if (opcode == 0x10) {
            const auto address = Word(bytes, offset + 1) & 0xffffu;
            const auto count = (length - 5) / 4;
            for (size_t index = 0; index < count; ++index) {
                gx.xf[address + index] = Word(bytes, offset + 5 + index * 4);
            }
        } else if (opcode == 0x61) {
            const auto word = Word(bytes, offset + 1);
            const auto address = word >> 24;
            const auto data = (gx.bp[address] & ~gx.bpMask) | (word & gx.bpMask);
            if (address == 0x52) {
                throw std::runtime_error("Native GX EFB copy requires a renderer/copy backend.");
            }
            if (address == 0x63 || address == 0x65) {
                throw std::runtime_error("Native GX texture/TLUT DMA requires a texture-memory backend.");
            }
            if (address == 0x45 && data != 2) {
                throw std::runtime_error("Unsupported native GX draw-done command.");
            }
            gx.bp[address] = data;
            gx.bpMask = address == 0xfe ? data : 0xffffffu;
            if (address == 0x45) gx.pePending |= 8u;
            if (address == 0x47 || address == 0x48) {
                gx.token = static_cast<uint16_t>(data);
                gx.cpToken = gx.token;
                if (address == 0x48) gx.pePending |= 4u;
            }
        } else if (opcode == 0x48) {
            ++gx.vertexCacheInvalidations;
        }
        ++gx.decodedCommands;
        offset += length;
    }
    bytes.erase(bytes.begin(), bytes.begin() + offset);
}

void ValidateRing(const PpcState& state, uint32_t base, uint32_t end, uint32_t pointer) {
    if ((base & 31u) != 0 || (end & 31u) != 0 || (pointer & 31u) != 0 ||
        end < base || pointer < base || pointer > end) {
        throw std::runtime_error("Invalid native GX FIFO ring bounds or pointer alignment.");
    }
    ppc_memory_offset(state, base, static_cast<size_t>(end) - base + 32);
}

void Drain(PpcState& state, NativeGx& gx) {
    if ((gx.control & 1u) == 0 || gx.fifo[4] == 0 || Breakpoint(gx)) return;
    ValidateRing(state, gx.fifo[0], gx.fifo[1], gx.fifo[6]);
    const uint32_t capacity = gx.fifo[1] - gx.fifo[0] + 32;
    if ((gx.fifo[4] & 31u) != 0 || gx.fifo[4] > capacity) {
        throw std::runtime_error("Invalid native GX FIFO byte count.");
    }
    while (gx.fifo[4] != 0 && !Breakpoint(gx)) {
        const auto offset = ppc_memory_offset(state, gx.fifo[6], 32);
        gx.commands.insert(gx.commands.end(), state.memory.begin() + offset,
                           state.memory.begin() + offset + 32);
        Decode(gx);
        gx.fifo[6] = gx.fifo[6] == gx.fifo[1] ? gx.fifo[0] : gx.fifo[6] + 32;
        gx.fifo[4] -= 32;
        if (gx.fifo[4] < gx.fifo[3]) gx.watermarkStatus |= 2u;
    }
}

void Gather(PpcState& state, NativeGx& gx, uint32_t value, unsigned width) {
    if (gx.writeGatherAddress != 0x0c008000u || (state.hid2 & 0x40000000u) == 0) {
        throw std::runtime_error("Native GX FIFO requires configured WPAR and enabled write gathering.");
    }
    ValidateRing(state, gx.cpuBase, gx.cpuEnd, gx.cpuWrite);
    if (gx.gathered.size() + width >= 32 && (gx.control & 0x10u) != 0 &&
        (gx.cpuBase != gx.fifo[0] || gx.cpuEnd != gx.fifo[1] ||
         gx.cpuWrite != gx.fifo[5] || gx.fifo[4] > gx.cpuEnd - gx.cpuBase)) {
        throw std::runtime_error("Native GX linked FIFO is mismatched or full.");
    }
    for (unsigned index = width; index != 0; --index) {
        gx.gathered.push_back(static_cast<uint8_t>(value >> ((index - 1) * 8)));
    }
    if (gx.gathered.size() < 32) return;
    // Publish one actual 32-byte write-gather burst to guest FIFO RAM.
    const auto offset = ppc_memory_offset(state, gx.cpuWrite, 32);
    std::copy_n(gx.gathered.begin(), 32, state.memory.begin() + offset);
    gx.gathered.erase(gx.gathered.begin(), gx.gathered.begin() + 32);
    gx.cpuWrite = gx.cpuWrite == gx.cpuEnd ? gx.cpuBase : gx.cpuWrite + 32;
    ++gx.bursts;
    if ((gx.control & 0x10u) != 0) {
        gx.fifo[5] = gx.cpuWrite;
        gx.fifo[4] += 32;
        if (gx.fifo[4] > gx.fifo[2]) gx.watermarkStatus |= 1u;
        Drain(state, gx);
    }
}

} // namespace

uint32_t NativeGxInterruptCause(const NativeGx& gx) {
    uint32_t cause = 0;
    if (((gx.watermarkStatus & 1u) != 0 && (gx.control & 4u) != 0) ||
        ((gx.watermarkStatus & 2u) != 0 && (gx.control & 8u) != 0) ||
        (Breakpoint(gx) && (gx.control & 0x20u) != 0)) cause |= 0x800u;
    if ((gx.peControl & 1u) != 0 && (gx.pePending & 4u) != 0) cause |= 0x200u;
    if ((gx.peControl & 2u) != 0 && (gx.pePending & 8u) != 0) cause |= 0x400u;
    return cause;
}

uint32_t NativeGxRead(const PpcState& state, uint32_t address, unsigned width) {
    const auto& gx = state.graphics.value();
    if (width == 4) {
        if (address == 0xcc00300cu) return gx.cpuBase;
        if (address == 0xcc003010u) return gx.cpuEnd;
        if (address == 0xcc003014u) return gx.cpuWrite;
    }
    if (width == 2) {
        if (address == 0xcc000000u) {
            const bool readIdle = gx.fifo[4] == 0 || (gx.control & 1u) == 0 || Breakpoint(gx);
            return gx.watermarkStatus | (readIdle ? 4u : 0u) |
                   (gx.commands.empty() ? 8u : 0u) | (Breakpoint(gx) ? 0x10u : 0u);
        }
        if (address == 0xcc000002u) return gx.control;
        if (address == 0xcc000006u) return gx.cpToken;
        if (address >= 0xcc000020u && address <= 0xcc00003eu && (address & 1u) == 0) {
            const auto index = (address - 0xcc000020u) / 4;
            return (gx.fifo[index] >> ((address & 2u) * 8)) & 0xffffu;
        }
        if (address == 0xcc00100au) return gx.peControl;
        if (address == 0xcc00100eu) return gx.token;
    }
    throw std::runtime_error("Unsupported native GX register read or access width.");
}

void NativeGxWrite(PpcState& state, uint32_t address, uint32_t value, unsigned width) {
    auto& gx = state.graphics.value();
    if (address == 0xcc008000u && (width == 1 || width == 2 || width == 4)) {
        Gather(state, gx, value, width);
        return;
    }
    if (width == 4) {
        if (address == 0xcc00300cu || address == 0xcc003010u || address == 0xcc003014u) {
            if ((value & ~0x03ffffffu) != 0 || !gx.gathered.empty()) {
                throw std::runtime_error("Unsupported native GX CPU FIFO reconfiguration.");
            }
            if (address == 0xcc00300cu) gx.cpuBase = value & ~31u;
            if (address == 0xcc003010u) gx.cpuEnd = value & ~31u;
            if (address == 0xcc003014u) gx.cpuWrite = value & ~31u;
            return;
        }
    }
    if (width == 2) {
        if (address == 0xcc000006u) {
            gx.cpToken = static_cast<uint16_t>(value);
            return;
        }
        if (address == 0xcc000002u) {
            if ((value & ~0x3fu) != 0) throw std::runtime_error("Unsupported native GX CP control bits.");
            auto candidate = gx;
            candidate.control = static_cast<uint16_t>(value);
            Drain(state, candidate);
            gx = std::move(candidate);
            return;
        }
        if (address == 0xcc000004u) {
            if ((value & ~3u) != 0) throw std::runtime_error("Unsupported native GX CP clear bits.");
            gx.watermarkStatus &= ~value;
            return;
        }
        if (address >= 0xcc000020u && address <= 0xcc00003eu && (address & 1u) == 0) {
            if ((gx.control & 1u) != 0) {
                throw std::runtime_error("Native GX FIFO registers require the CP reader disabled.");
            }
            const auto index = (address - 0xcc000020u) / 4;
            const auto shift = (address & 2u) * 8;
            const uint32_t mask = shift == 0 ? 0xffe0u : (index == 4 ? 0xffffu : 0x03ffu);
            if (shift != 0 && (value & ~mask) != 0) {
                throw std::runtime_error("Invalid native GX FIFO register bits.");
            }
            gx.fifo[index] = (gx.fifo[index] & ~(0xffffu << shift)) | ((value & mask) << shift);
            return;
        }
        if (address == 0xcc00100au) {
            if ((value & ~15u) != 0) throw std::runtime_error("Unsupported native GX PE control bits.");
            gx.pePending &= ~value;
            gx.peControl = static_cast<uint16_t>(value & 3u);
            return;
        }
    }
    throw std::runtime_error("Unsupported native GX register write or access width.");
}

void NativeGxSetWriteGatherAddress(PpcState& state, uint32_t value) {
    if (state.memoryMap != PpcMemoryMap::GameCubeRam || !state.graphics ||
        value != 0x0c008000u || !state.graphics->gathered.empty()) {
        throw std::runtime_error("PowerPC WPAR requires a native GX FIFO and an empty gather buffer.");
    }
    state.graphics->writeGatherAddress = value;
}
