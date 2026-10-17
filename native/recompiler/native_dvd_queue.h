#ifndef HMAWL_NATIVE_DVD_QUEUE_H
#define HMAWL_NATIVE_DVD_QUEUE_H

#include "native_dispatch.h"
#include "native_dvd_reader.h"

#include <vector>

class NativeDvdQueue {
public:
    NativeDvdQueue(NativeDvdReader& reader, NativeDispatcher& dispatcher, PpcState& state);
    NativeDvdQueue(const NativeDvdQueue&) = delete;
    NativeDvdQueue& operator=(const NativeDvdQueue&) = delete;

    void Submit(PpcState& state);
    void Cancel(PpcState& state);
    // Complete at most one queued request at a cooperative guest safe point.
    bool Pump();
    size_t pending() const { return requests_.size(); }

private:
    struct Request {
        uint32_t info;
        size_t identity;
        uint32_t destination;
        uint32_t length;
        uint32_t offset;
        uint32_t start;
        uint32_t size;
        uint32_t callback;
        uint32_t fileCallback;
        uint32_t priority;
    };
    void CheckOwner(const PpcState& state) const;
    void CheckCallback(uint32_t callback) const;
    void CheckRequest(const Request& request) const;
    void Invoke(uint32_t callback, uint32_t result, uint32_t info);
    NativeDvdReader& reader_;
    NativeDispatcher& dispatcher_;
    PpcState& state_;
    std::vector<Request> requests_;
    bool pumping_ = false;
};

inline void NativeSubmitDvdRead(PpcState& state) {
    if (!state.submitDvdRead) {
        throw std::runtime_error("Native DVD read service is not installed.");
    }
    state.submitDvdRead(state);
    state.pc = state.lr;
}

inline void NativeCancelDvdRead(PpcState& state) {
    if (!state.cancelDvdRead) {
        throw std::runtime_error("Native DVD cancel service is not installed.");
    }
    state.cancelDvdRead(state);
    state.pc = state.lr;
}

#endif
