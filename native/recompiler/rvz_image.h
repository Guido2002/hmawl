#ifndef HMAWL_RVZ_IMAGE_H
#define HMAWL_RVZ_IMAGE_H

#include <cstdint>
#include <array>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

struct RvzMetadata {
    std::string discId;
    std::string title;
    uint8_t discNumber = 0;
    uint8_t discVersion = 0;
    uint32_t compression = 0;
    uint32_t chunkSize = 0;
    uint64_t discSize = 0;
    uint64_t fileSize = 0;
    uint32_t rawGroupCount = 0;
    uint32_t compressedGroupCount = 0;
};

RvzMetadata InspectRvz(const std::filesystem::path& path);

class RvzImage {
public:
    explicit RvzImage(const std::filesystem::path& path);
    const RvzMetadata& metadata() const { return metadata_; }
    std::vector<uint8_t> Read(uint64_t offset, size_t size);

private:
    struct RawEntry {
        uint64_t offset;
        uint64_t size;
        uint32_t firstGroup;
        uint32_t groupCount;
    };
    struct Group {
        uint64_t offset;
        uint32_t size;
        uint32_t packedSize;
        bool compressed;
    };
    std::vector<uint8_t> ReadFile(uint64_t offset, size_t size);
    std::vector<uint8_t> Decode(uint64_t offset, size_t size,
                                size_t expectedSize, bool compressed);

    std::ifstream input_;
    RvzMetadata metadata_;
    std::array<uint8_t, 0x80> discHeader_ = {};
    std::vector<RawEntry> rawEntries_;
    std::vector<Group> groups_;
    size_t cachedGroup_ = static_cast<size_t>(-1);
    uint64_t cachedDiscOffset_ = 0;
    std::vector<uint8_t> cachedBytes_;
};

#endif
