#ifndef HMAWL_NATIVE_CACHE_H
#define HMAWL_NATIVE_CACHE_H

#include "ppc_state.h"

#include <atomic>

// Guest RAM aliases share coherent host storage; there is no separate DMA cache.
inline void NativeFlushDataRange(PpcState& state) {
    const uint32_t length = state.gpr[4];
    ppc_set_cr_field(state, 0, false, length != 0, length == 0);
    if (length != 0) {
        const uint32_t address = state.gpr[3];
        const uint64_t end = static_cast<uint64_t>(address) + length;
        if (end > UINT32_MAX) {
            throw std::runtime_error("Native data cache flush range overflows.");
        }
        const uint64_t alignedEnd = (end + 31u) & ~uint64_t{31};
        const uint32_t alignedStart = address & ~31u;
        ppc_memory_offset(state, alignedStart,
                          static_cast<size_t>(alignedEnd - alignedStart));
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }
    state.pc = state.lr;
}

#endif
