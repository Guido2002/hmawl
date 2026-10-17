#ifndef HMAWL_NATIVE_EXI_BUS_H
#define HMAWL_NATIVE_EXI_BUS_H

#include "ppc_state.h"

inline uint32_t TransferEmptyExternalExi(const ExiImmediateRequest& request) {
    if (request.channel >= 3 || request.device >= 3 || request.length == 0 ||
        request.length > 4 || request.direction > 2) {
        throw std::runtime_error("Invalid native EXI peripheral transaction.");
    }
    if ((request.channel == 0 && (request.device == 0 || request.device == 2)) ||
        (request.channel == 1 && request.device == 0)) {
        // Explicitly empty external ports, not an unidentified connected device.
        return 0;
    }
    throw std::runtime_error("Native EXI internal peripheral is not implemented (RTC/SRAM/ROM or AD16).");
}

#endif
