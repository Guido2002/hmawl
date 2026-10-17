#include "native_vi.h"
#include "ppc_state.h"

#include <stdexcept>

namespace {

bool Configuration(unsigned offset) {
    return offset <= 0x2a || (offset >= 0x48 && offset <= 0x66) ||
        offset == 0x6c || offset == 0x70 || offset == 0x72 || offset == 0x74;
}

bool Interrupt(unsigned offset) {
    return offset >= 0x30 && offset <= 0x3e;
}

void Validate(unsigned offset, uint16_t value) {
    if (offset == 2 && value != 0 && value != 1 && value != 2) {
        throw std::runtime_error("Native VI supports only NTSC interlaced control/reset.");
    }
    if (offset == 0x6c && value != 0) {
        throw std::runtime_error("Native VI progressive clock is not implemented.");
    }
    if (Interrupt(offset) && (offset & 2) == 0 && (value & ~0x93ffu)) {
        throw std::runtime_error("Invalid native VI display-interrupt control bits.");
    }
    if (Interrupt(offset) && (offset & 2) != 0 && (value & ~0x7ffu)) {
        throw std::runtime_error("Invalid native VI horizontal interrupt position.");
    }
}

uint16_t ReadHalf(const NativeVi& vi, unsigned offset) {
    if (offset == 0x2c || offset == 0x2e) {
        const auto halfLine = vi.registers[3] & 0x3ffu;
        if (!vi.clockStarted || halfLine != 429) {
            throw std::runtime_error("Native VI beam read requires configured NTSC timing and a time base.");
        }
        const uint64_t pixels = vi.elapsedTicks / 3;
        const uint64_t framePixels = uint64_t(halfLine) * 1050;
        const auto position = pixels % framePixels;
        return static_cast<uint16_t>(offset == 0x2c ? position / (halfLine * 2) + 1 :
                                                         position % (halfLine * 2) + 1);
    }
    if (offset == 0x6e) return 0; // Explicit disconnected digital-cable pins.
    if (Configuration(offset) || Interrupt(offset)) return vi.registers[offset / 2];
    throw std::runtime_error("Unsupported native VI register read.");
}

void WriteHalf(NativeVi& vi, unsigned offset, uint16_t value) {
    if (!Configuration(offset) && !Interrupt(offset)) {
        throw std::runtime_error("Unsupported native VI register write.");
    }
    Validate(offset, value);
    if (offset == 2 && value == 2) {
        for (unsigned index = 0x30 / 2; index < 0x40 / 2; ++index) vi.registers[index] = 0;
        vi.registers[1] = 0;
        vi.clockStarted = false;
        vi.lastTicks = vi.elapsedTicks = vi.fields = 0;
        ++vi.resets;
        ++vi.writes;
        return;
    }
    if (offset == 6 && vi.clockStarted && value != vi.registers[3]) {
        throw std::runtime_error("Native VI live scan-clock reconfiguration is not implemented.");
    }
    if (Interrupt(offset) && (offset & 2) == 0) {
        // The status latch is cleared by writing zero, never set by software.
        value = (value & ~0x8000u) | (vi.registers[offset / 2] & value & 0x8000u);
    }
    if (offset == 2 && vi.registers[1] != value) {
        vi.clockStarted = false;
        vi.elapsedTicks = 0;
    }
    vi.registers[offset / 2] = value;
    ++vi.writes;
}

} // namespace

uint32_t NativeViRead(const PpcState& state, uint32_t address, unsigned width) {
    if (!NativeViAddress(address) || (width != 1 && width != 2 && width != 4) || address % width ||
        address + width > 0xcc002078u) {
        throw std::runtime_error("Unsupported native VI read access width/alignment.");
    }
    const auto& vi = state.video.value();
    const auto offset = address - 0xcc002000u;
    if (width == 1) {
        const auto half = ReadHalf(vi, offset & ~1u);
        return (address & 1u) ? half & 0xffu : half >> 8;
    }
    const auto high = ReadHalf(vi, offset);
    return width == 2 ? high : (uint32_t(high) << 16) | ReadHalf(vi, offset + 2);
}

void NativeViWrite(PpcState& state, uint32_t address, uint32_t value, unsigned width) {
    if (!NativeViAddress(address) || (width != 2 && width != 4) || address % width ||
        address + width > 0xcc002078u) {
        throw std::runtime_error("Unsupported native VI write access width/alignment.");
    }
    auto candidate = state.video.value();
    const auto offset = address - 0xcc002000u;
    WriteHalf(candidate, offset, static_cast<uint16_t>(width == 2 ? value : value >> 16));
    if (width == 4) WriteHalf(candidate, offset + 2, static_cast<uint16_t>(value));
    state.video = candidate;
}

void NativeViAdvance(NativeVi& vi, uint64_t ticks) {
    if ((vi.registers[1] & 1) == 0) return;
    if ((vi.registers[3] & 0x3ffu) != 429) {
        throw std::runtime_error("Native VI scan timing requires NTSC half-line width 429.");
    }
    for (unsigned offset = 0x30; offset <= 0x3c; offset += 4) {
        const auto high = vi.registers[offset / 2];
        const uint32_t vertical = high & 0x3ffu, horizontal = vi.registers[offset / 2 + 1];
        if ((high & 0x1000u) &&
            (vertical == 0 || vertical > 525 || horizontal == 0 || horizontal > 858)) {
            throw std::runtime_error("Native VI enabled interrupt has an invalid NTSC beam position.");
        }
    }
    if (!vi.clockStarted) {
        vi.lastTicks = ticks;
        vi.clockStarted = true;
        return;
    }
    if (ticks < vi.lastTicks) throw std::runtime_error("Native VI time base moved backwards.");
    const auto delta = ticks - vi.lastTicks;
    if (delta > UINT64_MAX - vi.elapsedTicks) {
        throw std::runtime_error("Native VI elapsed time overflow.");
    }
    constexpr uint64_t halfField = 429 * 525 * 3;
    constexpr uint64_t frame = halfField * 2;
    const auto before = vi.elapsedTicks;
    const auto after = before + delta;
    for (unsigned offset = 0x30; offset <= 0x3c; offset += 4) {
        auto& high = vi.registers[offset / 2];
        const uint32_t vertical = high & 0x3ffu, horizontal = vi.registers[offset / 2 + 1];
        if (vertical == 0 || vertical > 525 || horizontal == 0 || horizontal > 858) continue;
        const uint64_t target = ((vertical - 1) * UINT64_C(858) + horizontal - 1) * 3;
        const uint64_t phase = before % frame;
        const uint64_t distance = target > phase ? target - phase : frame - phase + target;
        if (delta >= distance) high |= 0x8000u;
    }
    vi.fields += after / halfField - before / halfField;
    vi.elapsedTicks = after;
    vi.lastTicks = ticks;
}

uint32_t NativeViInterruptCause(const NativeVi& vi) {
    for (unsigned offset = 0x30; offset <= 0x3c; offset += 4) {
        if ((vi.registers[offset / 2] & 0x9000u) == 0x9000u) return 0x100;
    }
    return 0;
}
