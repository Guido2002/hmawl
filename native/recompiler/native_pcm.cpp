#include "native_pcm.h"
#include "ppc_state.h"

namespace {

uint32_t SubmitBuffer(PpcState& state, uint16_t blocks, uint32_t rate) {
    auto& pcm = *state.pcm;
    const uint32_t frames = (blocks & 0x7fffu) * 8u;
    if (!frames) throw std::runtime_error("Native PCM DMA requires a nonempty buffer.");
    const auto offset = ppc_memory_offset(state, state.dspHardware->audioDmaAddress, frames * 4);
    if (frames > UINT64_MAX - pcm.submittedFrames) throw std::runtime_error("Native PCM accounting overflow.");
    std::vector<int16_t> samples(frames * 2);
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto value = uint16_t((uint16_t(state.memory[offset + 2 * i]) << 8) |
                                   state.memory[offset + 2 * i + 1]);
        samples[i] = static_cast<int16_t>(value);
    }
    state.submitPcm(samples, rate);
    return frames;
}

} // namespace

void NativePcmAdvance(PpcState& state, uint64_t ticks) {
    if (!state.pcm || !state.pcm->active) return;
    if (!state.dspHardware || !state.dspControl || !state.aiControl || !state.submitPcm || !state.stopPcm) {
        throw std::runtime_error("Active native PCM DMA lost its hardware or playback backend.");
    }
    auto& pcm = *state.pcm;
    if (!pcm.remainingBlocks) throw std::runtime_error("Active native PCM DMA has no remaining blocks.");
    if (ticks < pcm.lastTicks) throw std::runtime_error("Native PCM clock moved backwards.");
    const uint64_t blockTicks = (*state.aiControl & 0x40u) ? 10116 : 6744;
    const auto elapsed = ticks - pcm.lastTicks;
    const auto fraction = elapsed % blockTicks + pcm.phase;
    auto blocks = elapsed / blockTicks + fraction / blockTicks;
    unsigned reloads = 0;
    while (blocks >= pcm.remainingBlocks) {
        if (++reloads > 1024) throw std::runtime_error("Native PCM DMA catch-up exceeds the bounded playback window.");
        if (pcm.completedBuffers == UINT64_MAX) throw std::runtime_error("Native PCM period accounting overflow.");
        const auto consumed = pcm.remainingBlocks;
        const auto frames = SubmitBuffer(state, state.dspHardware->audioDmaBlocks, pcm.sampleRate);
        pcm.submittedFrames += frames;
        pcm.remainingBlocks = static_cast<uint16_t>(frames / 8);
        blocks -= consumed;
        pcm.lastTicks = ticks - blocks * blockTicks - fraction % blockTicks;
        pcm.phase = 0;
        ++pcm.completedBuffers;
        *state.dspControl |= 8u;
    }
    pcm.remainingBlocks -= static_cast<uint16_t>(blocks);
    pcm.lastTicks = ticks;
    pcm.phase = fraction % blockTicks;
}

void NativePcmWrite(PpcState& state, uint32_t address, uint16_t value) {
    if (!state.dspHardware) throw std::runtime_error("Native PCM requires its DSP register profile.");
    if (address != 0xcc005030u && address != 0xcc005032u && address != 0xcc005036u) {
        throw std::runtime_error("Invalid native PCM register write.");
    }
    auto& dsp = *state.dspHardware;
    const bool enable = address == 0xcc005036u && (value & 0x8000u);
    if (enable && (!state.pcm || !state.submitPcm || !state.stopPcm)) {
        throw std::runtime_error("Audio DMA enable requires a native PCM playback backend.");
    }
    if (enable && (!(value & 0x7fffu) || !state.aiControl)) {
        throw std::runtime_error("Native PCM DMA requires a nonempty buffer and AI rate profile.");
    }
    if (state.pcm && state.pcm->active) {
        if (!state.readTimeBase) throw std::runtime_error("Native PCM requires a time-base backend.");
        NativePcmAdvance(state, state.readTimeBase());
    }
    if (address == 0xcc005030u) {
        dsp.audioDmaAddress = (dsp.audioDmaAddress & 0xffffu) | (uint32_t(value & 0x3ffu) << 16);
    } else if (address == 0xcc005032u) {
        dsp.audioDmaAddress = (dsp.audioDmaAddress & 0xffff0000u) | (value & 0xffe0u);
    } else if (address == 0xcc005036u) {
        if (enable && !state.pcm->active) {
            if (!state.readTimeBase) throw std::runtime_error("Native PCM requires a time-base backend.");
            const auto ticks = state.readTimeBase();
            auto& pcm = *state.pcm;
            if (pcm.starts == UINT64_MAX) throw std::runtime_error("Native PCM start accounting overflow.");
            const uint32_t divisor = (*state.aiControl & 0x40u) ? 3372 : 2248;
            const auto rate = (108000000u + divisor / 2) / divisor;
            const auto frames = SubmitBuffer(state, value, rate);
            pcm.sampleRate = rate;
            pcm.submittedFrames += frames;
            pcm.remainingBlocks = static_cast<uint16_t>(frames / 8);
            dsp.audioDmaBlocks = value;
            pcm.active = true;
            pcm.lastTicks = ticks;
            pcm.phase = 0;
            ++pcm.starts;
        } else {
            if (!enable && state.pcm && state.pcm->active) {
                state.stopPcm();
                state.pcm->active = false;
                state.pcm->remainingBlocks = 0;
                state.pcm->phase = 0;
            }
            dsp.audioDmaBlocks = value;
        }
    }
}
