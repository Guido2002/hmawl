#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <mmsystem.h>

#include "native_windows_audio.h"
#include "ppc_state.h"

#include <deque>
#include <iostream>
#include <memory>

namespace {

void Check(MMRESULT result, const char* operation) {
    if (result != MMSYSERR_NOERROR) {
        throw std::runtime_error(std::string("Windows PCM ") + operation + " failed: MMRESULT=" +
                                 std::to_string(result) + ".");
    }
}

class WindowsPcmOutput {
    struct Buffer {
        std::vector<int16_t> samples;
        WAVEHDR header{};
    };
    HWAVEOUT device_ = nullptr;
    uint32_t rate_ = 0;
    std::deque<std::unique_ptr<Buffer>> buffers_;

    void Reclaim() {
        while (!buffers_.empty() && (buffers_.front()->header.dwFlags & WHDR_DONE)) {
            Check(waveOutUnprepareHeader(device_, &buffers_.front()->header, sizeof(WAVEHDR)), "unprepare");
            buffers_.pop_front();
        }
    }

public:
    ~WindowsPcmOutput() {
        if (!device_) return;
        const auto report = [](MMRESULT result, const char* operation) {
            if (result != MMSYSERR_NOERROR) {
                std::cerr << "Windows PCM cleanup " << operation << " failed: MMRESULT=" << result << ".\n";
            }
        };
        report(waveOutReset(device_), "reset");
        for (const auto& buffer : buffers_) {
            report(waveOutUnprepareHeader(device_, &buffer->header, sizeof(WAVEHDR)), "unprepare");
        }
        report(waveOutClose(device_), "close");
    }

    void Submit(const std::vector<int16_t>& samples, uint32_t rate) {
        if (samples.empty() || (samples.size() & 1u) || samples.size() > 524272 ||
            (rate != 32028 && rate != 48043)) {
            throw std::runtime_error("Invalid Windows stereo PCM sample buffer or rate.");
        }
        Reclaim();
        if (device_ && rate != rate_) {
            if (!buffers_.empty()) throw std::runtime_error("Windows PCM live rate changes are not implemented.");
            Check(waveOutClose(device_), "close for rate change");
            device_ = nullptr;
        }
        if (!device_) {
            WAVEFORMATEX format{};
            format.wFormatTag = WAVE_FORMAT_PCM;
            format.nChannels = 2;
            format.nSamplesPerSec = rate;
            format.wBitsPerSample = 16;
            format.nBlockAlign = 4;
            format.nAvgBytesPerSec = rate * 4;
            Check(waveOutOpen(&device_, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL), "open");
            rate_ = rate;
        }
        if (buffers_.size() >= 256) throw std::runtime_error("Windows PCM playback queue is full; no samples were discarded.");
        auto buffer = std::make_unique<Buffer>();
        buffer->samples = samples;
        buffer->header.lpData = reinterpret_cast<LPSTR>(buffer->samples.data());
        buffer->header.dwBufferLength = static_cast<DWORD>(samples.size() * sizeof(int16_t));
        Check(waveOutPrepareHeader(device_, &buffer->header, sizeof(WAVEHDR)), "prepare");
        buffers_.push_back(std::move(buffer));
        const auto result = waveOutWrite(device_, &buffers_.back()->header, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR) {
            Check(waveOutUnprepareHeader(device_, &buffers_.back()->header, sizeof(WAVEHDR)), "unprepare rejected buffer");
            buffers_.pop_back();
            Check(result, "write");
        }
    }

    void Stop() {
        if (!device_) return;
        Check(waveOutReset(device_), "stop");
        Reclaim();
    }
};

} // namespace

void BindNativeWindowsPcm(PpcState& state) {
    if (state.pcm || state.submitPcm || state.stopPcm) throw std::runtime_error("Native PCM backend is already bound.");
    auto output = std::make_shared<WindowsPcmOutput>();
    state.pcm.emplace();
    state.submitPcm = [output](const std::vector<int16_t>& samples, uint32_t rate) { output->Submit(samples, rate); };
    state.stopPcm = [output] { output->Stop(); };
}
