#ifndef HMAWL_NATIVE_DSP_INIT_H
#define HMAWL_NATIVE_DSP_INIT_H

#include "ppc_state.h"

inline void NativeDspInitStep(PpcDspHardware& dsp, uint16_t pc, uint32_t& budget) {
    dsp.instructionPc = pc;
    if (budget == 0) throw std::runtime_error("Native DSP initialization instruction budget exceeded.");
    --budget;
    ++dsp.executedInstructions;
}

inline uint16_t NativeDspReadOutputHigh(const PpcDspHardware& dsp) {
    return static_cast<uint16_t>(dsp.fromDspMailbox >> 16);
}

inline void NativeDspWriteOutput(PpcDspHardware& dsp, uint16_t address, uint16_t value) {
    if ((dsp.fromDspMailbox & 0x80000000u) != 0) {
        throw std::runtime_error("Native DSP attempted to overwrite an unread mailbox.");
    }
    if (address == 0xfffcu) {
        dsp.fromDspMailbox = (dsp.fromDspMailbox & 0xffffu) |
                             (static_cast<uint32_t>(value & 0x7fffu) << 16);
    } else if (address == 0xfffdu) {
        dsp.fromDspMailbox = (dsp.fromDspMailbox & 0xffff0000u) | value | 0x80000000u;
        ++dsp.sentMail;
    } else {
        throw std::runtime_error("Unsupported native DSP initialization output register.");
    }
}

#endif
