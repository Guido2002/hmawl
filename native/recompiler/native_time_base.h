#ifndef HMAWL_NATIVE_TIME_BASE_H
#define HMAWL_NATIVE_TIME_BASE_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <stdexcept>

constexpr uint64_t kGameCubeTimeBaseHz = 40500000u;

inline uint64_t GameCubeTicksFromNanoseconds(int64_t nanoseconds) {
    if (nanoseconds < 0) {
        throw std::runtime_error("Native time base cannot run backwards.");
    }
    const auto elapsed = static_cast<uint64_t>(nanoseconds);
    return (elapsed / 1000000000u) * kGameCubeTimeBaseHz +
           ((elapsed % 1000000000u) * kGameCubeTimeBaseHz) / 1000000000u;
}

inline std::function<uint64_t()> MakeNativeTimeBase() {
    const auto epoch = std::chrono::steady_clock::now();
    return [epoch] {
        return GameCubeTicksFromNanoseconds(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - epoch).count());
    };
}

#endif
