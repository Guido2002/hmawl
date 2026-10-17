#ifndef HMAWL_DOL_IMAGE_H
#define HMAWL_DOL_IMAGE_H

#include "ppc_state.h"
#include <cstddef>
#include <cstdint>
#include <vector>

struct DolSection {
    uint32_t fileOffset;
    uint32_t address;
    uint32_t size;
    bool executable;
};

class DolImage {
public:
    static DolImage Parse(std::vector<uint8_t> bytes);

    const std::vector<DolSection>& sections() const { return sections_; }
    uint32_t entryPoint() const { return entryPoint_; }
    uint32_t bssAddress() const { return bssAddress_; }
    uint32_t bssSize() const { return bssSize_; }

    std::vector<uint32_t> ReadWords(uint32_t address, uint32_t size) const;
    PpcState CreateGameCubeState() const;

private:
    std::vector<uint8_t> bytes_;
    std::vector<DolSection> sections_;
    uint32_t entryPoint_ = 0;
    uint32_t bssAddress_ = 0;
    uint32_t bssSize_ = 0;
};

#endif
