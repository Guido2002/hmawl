#include "native_dvd_queue.h"

#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>

NativeDvdQueue::NativeDvdQueue(NativeDvdReader& reader, NativeDispatcher& dispatcher,
                               PpcState& state)
    : reader_(reader), dispatcher_(dispatcher), state_(state) {}

void NativeDvdQueue::CheckOwner(const PpcState& state) const {
    if (&state != &state_) {
        throw std::runtime_error("Native DVD queue used with a different guest state.");
    }
}

void NativeDvdQueue::CheckCallback(uint32_t callback) const {
    if (callback != 0 && ((callback & 3u) != 0 || !dispatcher_.Contains(callback))) {
        throw std::runtime_error("Native DVD callback has no registered native translation.");
    }
}

void NativeDvdQueue::CheckRequest(const Request& request) const {
    if (ppc_read_u32(state_, request.info + 8) != 1 ||
        ppc_read_u32(state_, request.info + 0xc) != 2 ||
        ppc_read_u32(state_, request.info + 0x10) != request.start + request.offset ||
        ppc_read_u32(state_, request.info + 0x14) != request.length ||
        ppc_read_u32(state_, request.info + 0x18) != request.destination ||
        ppc_read_u32(state_, request.info + 0x20) != 0 ||
        ppc_read_u32(state_, request.info + 0x28) != request.callback ||
        ppc_read_u32(state_, request.info + 0x30) != request.start ||
        ppc_read_u32(state_, request.info + 0x34) != request.size ||
        ppc_read_u32(state_, request.info + 0x38) != request.fileCallback) {
        throw std::runtime_error("Native DVD queued command metadata changed before completion.");
    }
}

void NativeDvdQueue::Invoke(uint32_t callback, uint32_t result, uint32_t info) {
    if (callback == 0) {
        return;
    }
    CheckCallback(callback);
    std::array<uint32_t, 32> gpr;
    std::copy(std::begin(state_.gpr), std::end(state_.gpr), gpr.begin());
    const auto fpr = state_.fpr;
    const auto fpscr = state_.fpscr;
    const auto gqr = state_.gqr;
    const auto msr = state_.msr, hid2 = state_.hid2, hid0 = state_.hid0;
    const auto l2cr = state_.l2cr;
    const auto performanceMonitor = state_.performanceMonitor;
    const auto cr = state_.cr, xer = state_.xer, lr = state_.lr;
    const auto ctr = state_.ctr, pc = state_.pc;
    const auto srr0 = state_.srr0, srr1 = state_.srr1;
    const auto systemCallActive = state_.systemCallActive;
    const auto fpuException = state_.fpuException;
    const auto sprg0 = state_.sprg0;
    const auto externalInterrupt = state_.externalInterrupt;
    const auto ramBats = state_.ramBats;
    const bool interrupts = state_.externalInterruptsEnabled;
    const auto restore = [&] {
        std::copy(gpr.begin(), gpr.end(), std::begin(state_.gpr));
        state_.fpr = fpr;
        state_.fpscr = fpscr;
        state_.gqr = gqr;
        state_.msr = msr;
        state_.hid2 = hid2;
        state_.hid0 = hid0;
        state_.l2cr = l2cr;
        state_.performanceMonitor = performanceMonitor;
        state_.cr = cr;
        state_.xer = xer;
        state_.lr = lr;
        state_.ctr = ctr;
        state_.pc = pc;
        state_.srr0 = srr0;
        state_.srr1 = srr1;
        state_.systemCallActive = systemCallActive;
        state_.fpuException = fpuException;
        state_.sprg0 = sprg0;
        state_.externalInterrupt = externalInterrupt;
        state_.ramBats = ramBats;
        state_.externalInterruptsEnabled = interrupts;
    };
    state_.gpr[3] = result;
    state_.gpr[4] = info;
    state_.externalInterruptsEnabled = false;
    try {
        if (state_.callGuest) {
            state_.callGuest(callback, state_);
        } else {
            dispatcher_.Execute(state_, callback);
        }
    } catch (...) {
        restore();
        throw;
    }
    restore();
}

void NativeDvdQueue::Submit(PpcState& state) {
    CheckOwner(state);
    const uint32_t info = state.gpr[3], destination = state.gpr[4], length = state.gpr[5];
    const uint32_t absolute = state.gpr[6], callback = state.gpr[7], priority = state.gpr[8];
    const size_t identity = ppc_memory_offset(state, info, 0x3c);
    if (priority > 3) {
        throw std::runtime_error("Native DVD priority must be in the SDK range 0..3.");
    }
    if (std::any_of(requests_.begin(), requests_.end(),
                    [&](const Request& request) { return request.identity == identity; })) {
        throw std::runtime_error("Native DVD command block is already queued.");
    }
    const uint32_t status = ppc_read_u32(state, info + 0xc);
    if (status != 0 && status != 10 && status != 0xffffffffu) {
        throw std::runtime_error("Native DVD command block is not in a terminal state.");
    }
    CheckCallback(callback);
    const uint32_t start = ppc_read_u32(state, info + 0x30);
    if (absolute < start) {
        throw std::runtime_error("Native DVD absolute read offset precedes the opened file.");
    }
    const uint32_t offset = absolute - start;
    reader_.ValidateOpenedFileRead(state, info, destination, length, offset);
    const size_t output = ppc_memory_offset(state, destination, length);
    if (length != 0 && output < identity + 0x3c && identity < output + length) {
        throw std::runtime_error("Native DVD destination overlaps its command block.");
    }
    for (const auto& request : requests_) {
        const size_t pendingOutput = ppc_memory_offset(state, request.destination, request.length);
        if ((identity < request.identity + 0x3c && request.identity < identity + 0x3c) ||
            (length != 0 && output < request.identity + 0x3c &&
             request.identity < output + length) ||
            (request.length != 0 && identity < pendingOutput + request.length &&
             pendingOutput < identity + 0x3c)) {
            throw std::runtime_error("Native DVD buffers overlap another queued command block.");
        }
    }
    requests_.push_back({info, identity, destination, length, offset, start,
                         ppc_read_u32(state, info + 0x34), callback,
                         ppc_read_u32(state, info + 0x38), priority});
    ppc_write_u32(state, info + 8, 1);
    ppc_write_u32(state, info + 0xc, 2);
    ppc_write_u32(state, info + 0x10, absolute);
    ppc_write_u32(state, info + 0x14, length);
    ppc_write_u32(state, info + 0x18, destination);
    ppc_write_u32(state, info + 0x20, 0);
    ppc_write_u32(state, info + 0x28, callback);
    state.gpr[3] = 1;
}

void NativeDvdQueue::Cancel(PpcState& state) {
    CheckOwner(state);
    const uint32_t info = state.gpr[3], callback = state.gpr[4];
    const size_t identity = ppc_memory_offset(state, info, 0x3c);
    CheckCallback(callback);
    const auto found = std::find_if(requests_.begin(), requests_.end(),
        [&](const Request& request) { return request.identity == identity; });
    if (found != requests_.end()) {
        const Request request = *found;
        CheckCallback(request.callback);
        CheckRequest(request);
        requests_.erase(found);
        ppc_write_u32(state, request.info + 0xc, 10);
        Invoke(request.callback, 0xfffffffdu, info);
    } else {
        const uint32_t status = ppc_read_u32(state, info + 0xc);
        if (status != 0 && status != 10 && status != 0xffffffffu) {
            throw std::runtime_error("Native DVD cancellation cannot handle an unowned active command.");
        }
    }
    Invoke(callback, 0, info);
    state.gpr[3] = 1;
}

bool NativeDvdQueue::Pump() {
    if (pumping_ || state_.callGuest) {
        throw std::runtime_error("Native DVD queue must be pumped at a non-reentrant guest safe point.");
    }
    if (requests_.empty() || !state_.externalInterruptsEnabled) {
        return false;
    }
    const auto next = std::min_element(requests_.begin(), requests_.end(),
        [](const Request& left, const Request& right) { return left.priority < right.priority; });
    const Request request = *next;
    CheckCallback(request.callback);
    requests_.erase(next);
    pumping_ = true;
    struct PumpGuard {
        bool& flag;
        ~PumpGuard() { flag = false; }
    } guard{pumping_};
    std::exception_ptr failure;
    try {
        CheckRequest(request);
        reader_.ReadOpenedFile(state_, request.info, request.destination,
                               request.length, request.offset);
        ppc_write_u32(state_, request.info + 0x20, request.length);
        ppc_write_u32(state_, request.info + 0xc, 0);
    } catch (const std::exception&) {
        failure = std::current_exception();
        ppc_write_u32(state_, request.info + 0xc, 0xffffffffu);
        ppc_write_u32(state_, request.info + 0x38, request.fileCallback);
    }
    Invoke(request.callback, failure ? 0xffffffffu : request.length, request.info);
    if (failure) {
        std::rethrow_exception(failure);
    }
    return true;
}
