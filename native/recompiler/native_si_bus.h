#ifndef HMAWL_NATIVE_SI_BUS_H
#define HMAWL_NATIVE_SI_BUS_H

#include "ppc_state.h"

inline SiTransferResult TransferDisconnectedSi(const SiTransferRequest& request) {
    if (request.channel >= 4 || request.output.empty() || request.output.size() > 128 ||
        request.inputLength == 0 || request.inputLength > 128) {
        throw std::runtime_error("Invalid native SI peripheral transaction.");
    }
    return {{}, 8u};
}

#endif
