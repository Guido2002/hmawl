#include "dol_image.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

constexpr size_t kDolHeaderSize = 0x100;
constexpr size_t kTextSectionCount = 7;
constexpr size_t kDataSectionCount = 11;
constexpr size_t kFileOffsets = 0x00;
constexpr size_t kLoadAddresses = 0x48;
constexpr size_t kSectionSizes = 0x90;
constexpr size_t kBssAddress = 0xd8;
constexpr size_t kBssSize = 0xdc;
constexpr size_t kEntryPoint = 0xe0;

uint32_t ReadBigEndian32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < sizeof(uint32_t)) {
        throw std::runtime_error("DOL header is truncated.");
    }
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<uint32_t>(bytes[offset + 3]);
}

} // namespace

DolImage DolImage::Parse(std::vector<uint8_t> bytes) {
    if (bytes.size() < kDolHeaderSize) {
        throw std::runtime_error("File is too small to contain a DOL header.");
    }

    DolImage image;
    image.bytes_ = std::move(bytes);

    for (size_t index = 0; index < kTextSectionCount + kDataSectionCount;
         ++index) {
        const size_t offset = ReadBigEndian32(
            image.bytes_, kFileOffsets + index * sizeof(uint32_t));
        const size_t address = ReadBigEndian32(
            image.bytes_, kLoadAddresses + index * sizeof(uint32_t));
        const size_t size = ReadBigEndian32(
            image.bytes_, kSectionSizes + index * sizeof(uint32_t));

        if (size == 0) {
            continue;
        }
        if (offset < kDolHeaderSize || offset > image.bytes_.size() ||
            size > image.bytes_.size() - offset) {
            throw std::runtime_error("DOL section " + std::to_string(index) +
                                     " extends beyond the input file.");
        }
        if (size > std::numeric_limits<uint32_t>::max() ||
            address > std::numeric_limits<uint32_t>::max() - size) {
            throw std::runtime_error("DOL section load address overflows.");
        }

        image.sections_.push_back(
            {static_cast<uint32_t>(offset), static_cast<uint32_t>(address),
             static_cast<uint32_t>(size), index < kTextSectionCount});
    }

    image.bssAddress_ = ReadBigEndian32(image.bytes_, kBssAddress);
    image.bssSize_ = ReadBigEndian32(image.bytes_, kBssSize);
    image.entryPoint_ = ReadBigEndian32(image.bytes_, kEntryPoint);
    if (image.sections_.empty()) {
        throw std::runtime_error("DOL image contains no file-backed sections.");
    }
    return image;
}

std::vector<uint32_t> DolImage::ReadWords(uint32_t address, uint32_t size) const {
    if ((address & 3u) != 0 || (size & 3u) != 0 || size == 0) {
        throw std::runtime_error(
            "PowerPC code address and size must be non-zero and 4-byte aligned.");
    }

    if (address > std::numeric_limits<uint32_t>::max() - size) {
        throw std::runtime_error("PowerPC code range overflows the address space.");
    }

    for (const DolSection& section : sections_) {
        const uint64_t rangeEnd = static_cast<uint64_t>(address) + size;
        const uint64_t sectionEnd =
            static_cast<uint64_t>(section.address) + section.size;
        if (!section.executable || address < section.address ||
            rangeEnd > sectionEnd) {
            continue;
        }

        const size_t fileOffset =
            section.fileOffset + static_cast<size_t>(address - section.address);
        std::vector<uint32_t> words;
        words.reserve(size / sizeof(uint32_t));
        for (uint32_t offset = 0; offset < size; offset += sizeof(uint32_t)) {
            words.push_back(ReadBigEndian32(bytes_, fileOffset + offset));
        }
        return words;
    }

    throw std::runtime_error("Requested code range is not inside a DOL text section.");
}

PpcState DolImage::CreateGameCubeState() const {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0);
    state.pc = entryPoint_;
    ReadWords(entryPoint_, 4);
    if (bssSize_ != 0) {
        ppc_memory_offset(state, bssAddress_, bssSize_);
    }
    std::vector<std::pair<size_t, size_t>> loadedRanges;
    for (const auto& section : sections_) {
        const size_t offset = ppc_memory_offset(state, section.address, section.size);
        const size_t end = offset + section.size;
        for (const auto& range : loadedRanges) {
            if (offset < range.second && range.first < end) {
                throw std::runtime_error("DOL sections overlap in GameCube RAM.");
            }
        }
        loadedRanges.push_back({offset, end});
        std::copy_n(bytes_.data() + section.fileOffset, section.size,
                    state.memory.data() + offset);
    }
    return state;
}
