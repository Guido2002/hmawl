#ifndef HMAWL_NATIVE_INTERRUPTS_H
#define HMAWL_NATIVE_INTERRUPTS_H

#include "ppc_state.h"

// Cooperative interrupt delivery only; privileged MSR state is not emulated.
inline void NativeDisableInterrupts(PpcState& state) {
    state.gpr[3] = state.externalInterruptsEnabled ? 1u : 0u;
    state.externalInterruptsEnabled = false;
    state.pc = state.lr;
}

inline void NativeEnableInterrupts(PpcState& state) {
    state.gpr[3] = state.externalInterruptsEnabled ? 1u : 0u;
    state.externalInterruptsEnabled = true;
    state.pc = state.lr;
}

inline void NativeRestoreInterrupts(PpcState& state) {
    const uint32_t enabled = state.gpr[3];
    ppc_set_cr_field(state, 0, (enabled & 0x80000000u) != 0,
                     enabled != 0 && (enabled & 0x80000000u) == 0, enabled == 0);
    state.gpr[3] = state.externalInterruptsEnabled ? 1u : 0u;
    state.externalInterruptsEnabled = enabled != 0;
    state.pc = state.lr;
}

#endif
