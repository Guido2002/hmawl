#include "native_ai.h"
#include "ppc_state.h"

void NativeAiAdvance(PpcState& state, uint64_t ticks) {
    if (!state.audio || !state.aiControl) throw std::runtime_error("Native AI requires its control profile.");
    auto& ai = *state.audio;
    if (ai.clockStarted && ticks < ai.lastTicks) throw std::runtime_error("Native AI clock moved backwards.");
    const auto elapsed = ai.clockStarted ? ticks - ai.lastTicks : 0;
    if ((*state.aiControl & 1u) != 0) {
        // NTSC GC audio clock: 108 MHz / (2248 or 3372), TB: 40.5 MHz.
        const uint64_t denominator = (*state.aiControl & 2u) ? 6744 : 10116;
        const uint64_t whole = elapsed / denominator;
        const uint64_t fraction = (elapsed % denominator) * 8 + ai.phase;
        const uint64_t samples = whole * 8 + fraction / denominator;
        if (samples > UINT64_MAX - ai.advancedSamples) {
            throw std::runtime_error("Native AI sample accounting overflow.");
        }
        const uint64_t distance = uint32_t(ai.interruptTiming - ai.samples);
        if (!(*state.aiControl & 0x10u) && samples != 0 &&
            samples >= (distance ? distance : UINT64_C(0x100000000))) {
            *state.aiControl |= 8u;
        }
        ai.samples += static_cast<uint32_t>(samples);
        ai.advancedSamples += samples;
        ai.phase = fraction % denominator;
    }
    ai.lastTicks = ticks;
    ai.clockStarted = true;
}

uint32_t NativeAiRead(const PpcState& state, uint32_t address, unsigned width) {
    if (!state.audio || !state.aiControl || width != 4 || (address & 3u)) {
        throw std::runtime_error("Invalid native AI register read.");
    }
    switch (address) {
    case 0xcc006c00u: return *state.aiControl;
    case 0xcc006c04u: return state.audio->volume;
    case 0xcc006c08u: return state.audio->samples;
    case 0xcc006c0cu: return state.audio->interruptTiming;
    default: throw std::runtime_error("Unknown native AI register read.");
    }
}

void NativeAiWrite(PpcState& state, uint32_t address, uint32_t value, unsigned width) {
    if (!state.audio || !state.aiControl || width != 4 || (address & 3u) ||
        (address == 0xcc006c00u && (value & ~0x7fu)) ||
        (address == 0xcc006c04u && value > 0xffffu)) {
        throw std::runtime_error("Invalid native AI register write or reserved bits.");
    }
    if (address < 0xcc006c00u || address > 0xcc006c0cu) {
        throw std::runtime_error("Unknown native AI register write.");
    }
    if (!state.readTimeBase) throw std::runtime_error("Native AI requires a time-base backend.");
    const auto ticks = state.readTimeBase();
    NativeAiAdvance(state, ticks);
    auto& ai = *state.audio;
    switch (address) {
    case 0xcc006c00u: {
        const auto previous = *state.aiControl;
        if ((previous ^ value) & 3u) ai.phase = 0;
        *state.aiControl = (value & 0x57u) | (previous & 8u & ~value);
        if (value & 0x20u) {
            ai.samples = 0;
            ai.phase = 0;
            ++ai.resets;
        }
        break;
    }
    case 0xcc006c04u: ai.volume = static_cast<uint16_t>(value); break;
    case 0xcc006c08u:
        ai.samples = value;
        ai.phase = 0;
        break;
    case 0xcc006c0cu: ai.interruptTiming = value; break;
    }
    ++ai.writes;
}

uint32_t NativeAiInterruptCause(const PpcState& state) {
    return state.audio && state.aiControl && ((*state.aiControl & 0xcu) == 0xcu) ? 0x20u : 0;
}
