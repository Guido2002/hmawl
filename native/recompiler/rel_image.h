#ifndef HMAWL_REL_IMAGE_H
#define HMAWL_REL_IMAGE_H

#include "ppc_state.h"

#include <array>
#include <cstdint>
#include <vector>

struct RelSection {
    uint32_t fileOffset;
    uint32_t size;
    bool executable;
};

struct RelRelocation {
    uint32_t importId;
    uint8_t patchSection;
    uint32_t patchOffset;
    uint8_t type;
    uint8_t targetSection;
    uint32_t addend;
};

class RelImage {
public:
    static RelImage Parse(std::vector<uint8_t> bytes);

    uint32_t id() const { return id_; }
    uint32_t alignment() const { return alignment_; }
    uint32_t bssAlignment() const { return bssAlignment_; }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    const std::vector<RelSection>& sections() const { return sections_; }
    const std::vector<uint32_t>& imports() const { return imports_; }
    const std::vector<RelRelocation>& relocations() const { return relocations_; }
    const std::array<uint8_t, 3>& entrySections() const { return entrySections_; }
    const std::array<uint32_t, 3>& entryOffsets() const { return entryOffsets_; }

private:
    uint32_t id_ = 0;
    uint32_t alignment_ = 0;
    uint32_t bssAlignment_ = 0;
    std::vector<uint8_t> bytes_;
    std::vector<RelSection> sections_;
    std::vector<uint32_t> imports_;
    std::vector<RelRelocation> relocations_;
    std::array<uint8_t, 3> entrySections_ = {};
    std::array<uint32_t, 3> entryOffsets_ = {};
};

struct RelMemoryRange {
    uint32_t address;
    uint32_t size;
};

struct RelPlacement {
    const RelImage* image;
    uint32_t address;
};

struct LinkedRelModule {
    uint32_t id;
    uint32_t address;
    uint32_t size;
    std::vector<uint32_t> sectionAddresses;
    uint32_t prolog;
    uint32_t epilog;
    uint32_t unresolved;
    size_t relocationCount;
};

// This links guest code/data, not native functions, and does not invoke entrypoints.
std::vector<LinkedRelModule> LinkRelModules(
    PpcState& state, const std::vector<RelPlacement>& placements,
    const std::vector<RelMemoryRange>& reserved);

#endif
