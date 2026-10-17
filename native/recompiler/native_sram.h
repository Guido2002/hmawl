#ifndef HMAWL_NATIVE_SRAM_H
#define HMAWL_NATIVE_SRAM_H

#include "native_exi_bus.h"

#include <filesystem>
#include <memory>
#include <utility>

using NativeSramBytes = std::array<uint8_t, 64>;

inline NativeSramBytes NewNativeSram() {
    NativeSramBytes bytes = {};
    bytes[0x13] = 0x2c; // English, NTSC, stereo; no console or memory-card identity.
    uint16_t sum = 0;
    uint16_t inverse = 0;
    for (unsigned offset = 0x0c; offset < 0x14; offset += 2) {
        const auto value = static_cast<uint16_t>((uint16_t{bytes[offset]} << 8) | bytes[offset + 1]);
        sum = static_cast<uint16_t>(sum + value);
        inverse = static_cast<uint16_t>(inverse + (value ^ 0xffffu));
    }
    bytes[0] = static_cast<uint8_t>(sum >> 8);
    bytes[1] = static_cast<uint8_t>(sum);
    bytes[2] = static_cast<uint8_t>(inverse >> 8);
    bytes[3] = static_cast<uint8_t>(inverse);
    return bytes;
}

NativeSramBytes LoadOrCreateNativeSram(const std::filesystem::path& path);

class NativeSramBus {
public:
    explicit NativeSramBus(NativeSramBytes bytes) : bytes_(std::move(bytes)) {}

    void Select(unsigned channel, uint32_t selected) {
        if (channel == 0) {
            selected_ = selected == 0x100u;
            readPending_ = false;
        }
    }

    uint32_t Immediate(const ExiImmediateRequest& request) {
        if (request.channel != 0 || request.device != 1) return TransferEmptyExternalExi(request);
        if (!selected_ || readPending_ || request.length != 4 ||
            request.direction != 1 || request.data != 0x20000100u) {
            throw std::runtime_error("Unsupported native SRAM/RTC/ROM command (only full SRAM reads implemented).");
        }
        readPending_ = true;
        return 0;
    }

    std::vector<uint8_t> Dma(const ExiDmaRequest& request) {
        if (!selected_ || !readPending_ || request.channel != 0 || request.device != 1 ||
            request.direction != 0 || request.length != bytes_.size() || !request.output.empty()) {
            throw std::runtime_error("Unsupported native SRAM DMA transaction.");
        }
        std::vector<uint8_t> input(bytes_.begin(), bytes_.end());
        readPending_ = false;
        return input;
    }

private:
    NativeSramBytes bytes_;
    bool selected_ = false;
    bool readPending_ = false;
};

inline void BindNativeSram(PpcState& state, NativeSramBytes bytes) {
    auto bus = std::make_shared<NativeSramBus>(std::move(bytes));
    state.exiDmaEnabled = true;
    state.selectExi = [bus](unsigned channel, uint32_t selected) { bus->Select(channel, selected); };
    state.transferExiImmediate = [bus](const ExiImmediateRequest& request) { return bus->Immediate(request); };
    state.transferExiDma = [bus](const ExiDmaRequest& request) { return bus->Dma(request); };
}

#endif
