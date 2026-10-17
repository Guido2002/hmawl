#include "native_dvd_reader.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

NativeDvdReader::NativeDvdReader(RvzImage& disc)
    : disc_(disc), files_(ReadGameCubeFileTable(disc)) {}

uint64_t NativeDvdReader::ValidateOpenedFileRead(
    const PpcState& state, uint32_t fileInfo, uint32_t destination,
    uint32_t length, uint32_t offset) const {
    ppc_memory_offset(state, fileInfo, 0x3c);
    const uint32_t start = ppc_read_u32(state, fileInfo + 0x30);
    const uint32_t size = ppc_read_u32(state, fileInfo + 0x34);
    const auto file = std::find_if(files_.begin(), files_.end(),
        [&](const DiscFileEntry& entry) { return entry.offset == start && entry.size == size; });
    if (file == files_.end()) {
        throw std::runtime_error("DVDFileInfo does not identify a validated disc file.");
    }
    const uint64_t end = uint64_t{offset} + length;
    // DVDReadPrio permits the final DMA read to extend fewer than 32 bytes past EOF.
    if (offset > size || offset > uint32_t{std::numeric_limits<int32_t>::max()} ||
        end > uint32_t{std::numeric_limits<int32_t>::max()} || end >= uint64_t{size} + 32) {
        throw std::runtime_error("DVD read range is outside the opened file and its DMA padding.");
    }
    ppc_memory_offset(state, destination, length);
    const uint64_t address = uint64_t{start} + offset;
    if (address > disc_.metadata().discSize || length > disc_.metadata().discSize - address) {
        throw std::runtime_error("DVD read extends beyond the disc.");
    }
    return address;
}

uint32_t NativeDvdReader::ReadOpenedFile(PpcState& state, uint32_t fileInfo,
                                        uint32_t destination, uint32_t length,
                                        uint32_t offset) {
    const uint64_t address = ValidateOpenedFileRead(state, fileInfo, destination, length, offset);
    const size_t output = ppc_memory_offset(state, destination, length);
    const auto bytes = disc_.Read(address, length);
    std::copy(bytes.begin(), bytes.end(), state.memory.begin() + output);
    return length;
}
