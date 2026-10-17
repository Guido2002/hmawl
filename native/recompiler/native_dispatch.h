#ifndef HMAWL_NATIVE_DISPATCH_H
#define HMAWL_NATIVE_DISPATCH_H

#include "rel_image.h"

#include <map>
#include <vector>

using NativeGuestFunction = void (*)(PpcState&);

struct NativeTranslation {
    uint32_t address;
    NativeGuestFunction function;
    std::vector<uint32_t> expectedWords;
};

struct NativeRelTranslation {
    uint32_t section;
    uint32_t offset;
    NativeTranslation translation;
};

class NativeDispatcher {
public:
    NativeDispatcher() = default;
    NativeDispatcher(const NativeDispatcher&) = delete;
    NativeDispatcher& operator=(const NativeDispatcher&) = delete;

    void Register(uint32_t moduleId, const PpcState& state,
                  const std::vector<NativeTranslation>& translations);
    void RegisterRel(const RelImage& image, const LinkedRelModule& module,
                     const PpcState& state,
                     const std::vector<NativeRelTranslation>& translations);
    void Unregister(uint32_t moduleId);
    bool Contains(uint32_t address) const;

    // Runs compiled functions only. The caller supplies LR as the return sentinel.
    void Execute(PpcState& state, uint32_t address,
                 size_t maxFunctionEntries = 100000, size_t maxCallDepth = 256);

private:
    struct Entry {
        uint32_t moduleId;
        NativeTranslation translation;
    };
    void CheckCode(const PpcState& state, const NativeTranslation& translation) const;
    void Run(PpcState& state, uint32_t address, uint32_t returnAddress,
             size_t& remaining, size_t depth, size_t maxDepth);
    std::map<uint32_t, Entry> entries_;
    bool executing_ = false;
};

#endif
