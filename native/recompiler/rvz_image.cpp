#include "rvz_image.h"
#include "sha1.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>
#include <zstd.h>

namespace {

constexpr size_t kRvzFileHeaderSize = 0x48;
constexpr size_t kRvzDiscHeaderMinimumSize = 0xdc;
constexpr size_t kDiscHeaderOffsetInRvz = 0x10;
constexpr size_t kDiscHeaderSize = 0x80;
constexpr uint32_t kGameCubeDiscType = 1;
constexpr uint32_t kCompressionNone = 0;
constexpr uint32_t kCompressionZstandard = 5;
constexpr uint32_t kMinimumChunkSize = 0x8000;
constexpr uint32_t kMaximumChunkSize = 0x4000000;

uint32_t ReadBigEndian32(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

uint64_t ReadBigEndian64(const uint8_t* bytes) {
    return (static_cast<uint64_t>(ReadBigEndian32(bytes)) << 32) |
           ReadBigEndian32(bytes + 4);
}

void ReadExact(std::ifstream& input, uint64_t offset, uint8_t* destination,
               size_t size, const std::string& description) {
    if (offset > static_cast<uint64_t>(
                     std::numeric_limits<std::streamoff>::max()) ||
        size > static_cast<size_t>(
                   std::numeric_limits<std::streamsize>::max())) {
        throw std::runtime_error(description + " is too large to read.");
    }

    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input || !input.read(reinterpret_cast<char*>(destination),
                              static_cast<std::streamsize>(size))) {
        throw std::runtime_error(description + " is truncated.");
    }
}

std::vector<uint8_t> Unpack(const std::vector<uint8_t>& packed,
                             size_t expectedSize, uint64_t discOffset) {
    std::vector<uint8_t> output;
    output.reserve(expectedSize);
    size_t cursor = 0;
    while (cursor < packed.size()) {
        if (packed.size() - cursor < 4) {
            throw std::runtime_error("RVZ packed segment header is truncated.");
        }
        const uint32_t header = ReadBigEndian32(packed.data() + cursor);
        cursor += 4;
        const uint32_t length = header & 0x7fffffffu;
        if (length > expectedSize - output.size()) {
            throw std::runtime_error("RVZ packed segment has an invalid output size.");
        }
        if ((header & 0x80000000u) == 0) {
            if (length > packed.size() - cursor) {
                throw std::runtime_error("RVZ packed literal is truncated.");
            }
            output.insert(output.end(), packed.begin() + cursor,
                          packed.begin() + cursor + length);
            cursor += length;
        } else {
            if (packed.size() - cursor < 68) {
                throw std::runtime_error("RVZ packed PRNG seed is truncated.");
            }
            std::array<uint32_t, 521> buffer = {};
            for (size_t i = 0; i < 17; ++i) {
                buffer[i] = ReadBigEndian32(packed.data() + cursor + i * 4);
            }
            cursor += 68;
            for (size_t i = 17; i < buffer.size(); ++i) {
                buffer[i] = (buffer[i - 17] << 23) ^
                            (buffer[i - 16] >> 9) ^ buffer[i - 1];
            }
            const auto advance = [&buffer]() {
                for (size_t i = 0; i < 32; ++i) {
                    buffer[i] ^= buffer[i + 489];
                }
                for (size_t i = 32; i < buffer.size(); ++i) {
                    buffer[i] ^= buffer[i - 32];
                }
            };
            for (unsigned i = 0; i < 4; ++i) {
                advance();
            }
            const size_t skip = static_cast<size_t>(
                (discOffset + output.size()) % 0x8000u);
            constexpr unsigned shifts[] = {24, 18, 8, 0};
            for (size_t i = 0; i < skip + length; ++i) {
                const size_t byte = i % (buffer.size() * 4);
                if (i != 0 && byte == 0) {
                    advance();
                }
                if (i >= skip) {
                    output.push_back(static_cast<uint8_t>(
                        buffer[byte / 4] >> shifts[byte % 4]));
                }
            }
        }
    }
    if (output.size() != expectedSize) {
        throw std::runtime_error("RVZ unpacked group size does not match its range.");
    }
    return output;
}

} // namespace

RvzMetadata InspectRvz(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open RVZ file: " + path.string());
    }

    std::array<uint8_t, kRvzFileHeaderSize> fileHeader = {};
    ReadExact(input, 0, fileHeader.data(), fileHeader.size(), "RVZ file header");
    if (fileHeader[0] != 'R' || fileHeader[1] != 'V' ||
        fileHeader[2] != 'Z' || fileHeader[3] != 1) {
        throw std::runtime_error("Input is not a supported RVZ v1 image.");
    }

    const uint32_t discHeaderSize = ReadBigEndian32(fileHeader.data() + 0x0c);
    const uint64_t discSize = ReadBigEndian64(fileHeader.data() + 0x24);
    const uint64_t declaredFileSize = ReadBigEndian64(fileHeader.data() + 0x2c);
    if (discHeaderSize < kRvzDiscHeaderMinimumSize ||
        discHeaderSize > 16 * 1024 * 1024) {
        throw std::runtime_error("RVZ disc header has an invalid size.");
    }

    std::array<uint8_t, kRvzDiscHeaderMinimumSize> discMetadata = {};
    ReadExact(input, kRvzFileHeaderSize, discMetadata.data(),
              discMetadata.size(), "RVZ disc metadata");
    const uint32_t discType = ReadBigEndian32(discMetadata.data());
    if (discType != kGameCubeDiscType) {
        throw std::runtime_error(
            "RVZ disc metadata does not identify a GameCube disc.");
    }

    RvzMetadata metadata;
    const uint8_t* gameHeader =
        discMetadata.data() + kDiscHeaderOffsetInRvz;
    metadata.discId.assign(reinterpret_cast<const char*>(gameHeader), 6);
    metadata.discNumber = gameHeader[6];
    metadata.discVersion = gameHeader[7];
    metadata.title.assign(reinterpret_cast<const char*>(gameHeader + 0x20),
                          0x40);
    const size_t titleEnd = metadata.title.find('\0');
    if (titleEnd != std::string::npos) {
        metadata.title.resize(titleEnd);
    }
    metadata.compression = ReadBigEndian32(discMetadata.data() + 4);
    metadata.chunkSize = ReadBigEndian32(discMetadata.data() + 0x0c);
    metadata.discSize = discSize;
    metadata.fileSize = declaredFileSize;
    metadata.rawGroupCount = ReadBigEndian32(discMetadata.data() + 0xb4);
    metadata.compressedGroupCount = ReadBigEndian32(discMetadata.data() + 0xc4);

    if (metadata.discId.size() != 6 || metadata.title.empty()) {
        throw std::runtime_error("RVZ contains an incomplete GameCube disc header.");
    }
    if (metadata.compression != kCompressionNone &&
        metadata.compression != kCompressionZstandard) {
        throw std::runtime_error(
            "RVZ metadata uses an unsupported compression algorithm.");
    }
    if (metadata.chunkSize < kMinimumChunkSize ||
        metadata.chunkSize > kMaximumChunkSize ||
        (metadata.chunkSize < 0x200000u
             ? (metadata.chunkSize & (metadata.chunkSize - 1u)) != 0
             : metadata.chunkSize % 0x200000u != 0)) {
        throw std::runtime_error("RVZ metadata contains an invalid chunk size.");
    }

    input.seekg(0, std::ios::end);
    const std::streamoff actualFileSize = input.tellg();
    if (actualFileSize < 0 ||
        (declaredFileSize != 0 &&
         static_cast<uint64_t>(actualFileSize) != declaredFileSize)) {
        throw std::runtime_error("RVZ file size does not match its header.");
    }

    return metadata;
}

RvzImage::RvzImage(const std::filesystem::path& path)
    : input_(path, std::ios::binary), metadata_(InspectRvz(path)) {
    if (!input_) {
        throw std::runtime_error("Could not open RVZ file: " + path.string());
    }
    input_.seekg(0, std::ios::end);
    const auto fileSize = input_.tellg();
    if (fileSize < 0) {
        throw std::runtime_error("Could not determine the RVZ file size.");
    }
    metadata_.fileSize = static_cast<uint64_t>(fileSize);
    const auto header = ReadFile(0, kRvzFileHeaderSize);
    const uint32_t version = ReadBigEndian32(header.data() + 4);
    const uint32_t compatible = ReadBigEndian32(header.data() + 8);
    if (version < 0x00030000u || compatible > 0x01000000u ||
        compatible > version) {
        throw std::runtime_error("RVZ format version is not supported.");
    }
    const auto disc = ReadFile(kRvzFileHeaderSize,
                              ReadBigEndian32(header.data() + 0x0c));
    const auto headerHash = Sha1(header.data(), 0x34);
    const auto discHash = Sha1(disc.data(), disc.size());
    if (!std::equal(headerHash.begin(), headerHash.end(), header.begin() + 0x34) ||
        !std::equal(discHash.begin(), discHash.end(), header.begin() + 0x10)) {
        throw std::runtime_error("RVZ header SHA-1 verification failed.");
    }
    if (ReadBigEndian32(disc.data() + 0x90) != 0 ||
        disc[0xd4] != 0 || metadata_.discSize < kDiscHeaderSize ||
        metadata_.discSize > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(
            "Only GameCube RVZ images with 32-bit disc offsets, no partitions and no compressor options are supported.");
    }
    std::copy_n(disc.data() + 0x10, discHeader_.size(), discHeader_.begin());
    constexpr uint64_t tableLimit = 64u * 1024u * 1024u;
    const uint64_t rawSize = static_cast<uint64_t>(metadata_.rawGroupCount) * 24u;
    const uint64_t groupSize =
        static_cast<uint64_t>(metadata_.compressedGroupCount) * 12u;
    if (rawSize == 0 || rawSize > tableLimit ||
        groupSize == 0 || groupSize > tableLimit) {
        throw std::runtime_error("RVZ table counts exceed supported limits.");
    }
    const bool compressed = metadata_.compression == kCompressionZstandard;
    const auto raw = Decode(ReadBigEndian64(disc.data() + 0xb8),
                            ReadBigEndian32(disc.data() + 0xc0),
                            static_cast<size_t>(rawSize), compressed);
    const auto groups = Decode(ReadBigEndian64(disc.data() + 0xc8),
                               ReadBigEndian32(disc.data() + 0xd0),
                               static_cast<size_t>(groupSize), compressed);
    for (size_t i = 0; i < groups.size(); i += 12) {
        const uint32_t sizeFlags = ReadBigEndian32(groups.data() + i + 4);
        Group group = {static_cast<uint64_t>(ReadBigEndian32(groups.data() + i)) * 4u,
                       sizeFlags & 0x7fffffffu,
                       ReadBigEndian32(groups.data() + i + 8),
                       (sizeFlags & 0x80000000u) != 0 && compressed};
        if (group.offset > metadata_.fileSize ||
            group.size > metadata_.fileSize - group.offset ||
            group.packedSize > static_cast<uint64_t>(metadata_.chunkSize) * 5u ||
            group.size > static_cast<uint64_t>(metadata_.chunkSize) * 5u + 1024u) {
            throw std::runtime_error("RVZ data group has an invalid file range or size.");
        }
        groups_.push_back(group);
    }
    for (size_t i = 0; i < raw.size(); i += 24) {
        const uint64_t offset = ReadBigEndian64(raw.data() + i);
        const uint64_t size = ReadBigEndian64(raw.data() + i + 8);
        const uint32_t first = ReadBigEndian32(raw.data() + i + 16);
        const uint32_t count = ReadBigEndian32(raw.data() + i + 20);
        if (offset > metadata_.discSize || size > metadata_.discSize - offset ||
            first > groups_.size() || count > groups_.size() - first) {
            throw std::runtime_error("RVZ raw entry has an invalid disc or group range.");
        }
        if (size == 0) {
            continue;
        }
        const uint64_t alignedOffset = offset - offset % 0x8000u;
        const uint64_t alignedSize = size + offset % 0x8000u;
        if ((alignedSize + metadata_.chunkSize - 1u) / metadata_.chunkSize != count) {
            throw std::runtime_error("RVZ raw entry group count does not match its size.");
        }
        rawEntries_.push_back({alignedOffset, alignedSize, first, count});
    }
    std::sort(rawEntries_.begin(), rawEntries_.end(),
              [](const RawEntry& a, const RawEntry& b) { return a.offset < b.offset; });
    uint64_t end = kDiscHeaderSize;
    for (const auto& entry : rawEntries_) {
        if (entry.offset > end ||
            (entry.offset < end && !(&entry == rawEntries_.data() && entry.offset == 0))) {
            throw std::runtime_error("RVZ raw entries contain a gap or overlap.");
        }
        end = entry.offset + entry.size;
    }
    if (end != metadata_.discSize) {
        throw std::runtime_error("RVZ raw entries do not cover the GameCube image.");
    }
}

std::vector<uint8_t> RvzImage::ReadFile(uint64_t offset, size_t size) {
    if (offset > metadata_.fileSize || size > metadata_.fileSize - offset) {
        throw std::runtime_error("RVZ read extends beyond the input file.");
    }
    std::vector<uint8_t> bytes(size);
    if (size != 0) {
        ReadExact(input_, offset, bytes.data(), size, "RVZ data");
    }
    return bytes;
}

std::vector<uint8_t> RvzImage::Decode(uint64_t offset, size_t size,
                                      size_t expectedSize, bool compressed) {
    if (size > static_cast<uint64_t>(expectedSize) * 2u + 1024u) {
        throw std::runtime_error("RVZ compressed block exceeds the supported size limit.");
    }
    auto bytes = ReadFile(offset, size);
    if (!compressed) {
        if (bytes.size() != expectedSize) {
            throw std::runtime_error("RVZ uncompressed block size mismatch.");
        }
        return bytes;
    }
    const size_t frameSize = ZSTD_findFrameCompressedSize(bytes.data(), bytes.size());
    if (ZSTD_isError(frameSize) || frameSize != bytes.size()) {
        throw std::runtime_error("RVZ Zstandard block is truncated or has trailing data.");
    }
    std::vector<uint8_t> decoded(expectedSize);
    const size_t result = ZSTD_decompress(decoded.data(), decoded.size(),
                                         bytes.data(), bytes.size());
    if (ZSTD_isError(result)) {
        throw std::runtime_error(std::string("RVZ Zstandard decompression failed: ") +
                                 ZSTD_getErrorName(result));
    }
    if (result != expectedSize) {
        throw std::runtime_error("RVZ decompressed block size mismatch.");
    }
    return decoded;
}

std::vector<uint8_t> RvzImage::Read(uint64_t offset, size_t size) {
    if (offset > metadata_.discSize || size > metadata_.discSize - offset) {
        throw std::runtime_error("RVZ read extends beyond the disc image.");
    }
    if (size == 0) {
        return {};
    }
    std::vector<uint8_t> output(size);
    size_t written = 0;
    if (offset < discHeader_.size()) {
        const size_t count = std::min(size, discHeader_.size() - static_cast<size_t>(offset));
        std::copy_n(discHeader_.data() + static_cast<size_t>(offset), count, output.data());
        offset += count;
        written += count;
    }
    while (written < size) {
        const auto entry = std::find_if(rawEntries_.begin(), rawEntries_.end(),
            [offset](const RawEntry& raw) {
                return offset >= raw.offset && offset - raw.offset < raw.size;
            });
        if (entry == rawEntries_.end()) {
            throw std::runtime_error("RVZ disc range is not mapped by a raw entry.");
        }
        const uint64_t relative = offset - entry->offset;
        const uint64_t groupOffset = relative / metadata_.chunkSize * metadata_.chunkSize;
        const size_t index = entry->firstGroup +
                             static_cast<size_t>(relative / metadata_.chunkSize);
        const size_t expected = static_cast<size_t>(
            std::min<uint64_t>(metadata_.chunkSize, entry->size - groupOffset));
        const uint64_t discOffset = entry->offset + groupOffset;
        if (cachedGroup_ != index || cachedDiscOffset_ != discOffset ||
            cachedBytes_.size() != expected) {
            const auto& group = groups_.at(index);
            std::vector<uint8_t> bytes;
            if (group.size == 0) {
                bytes.resize(expected, 0);
            } else {
                bytes = Decode(group.offset, group.size,
                               group.packedSize == 0 ? expected : group.packedSize,
                               group.compressed);
                if (group.packedSize != 0) {
                    bytes = Unpack(bytes, expected, discOffset);
                }
            }
            cachedBytes_ = std::move(bytes);
            cachedGroup_ = index;
            cachedDiscOffset_ = discOffset;
        }
        const size_t within = static_cast<size_t>(relative % metadata_.chunkSize);
        const size_t count = std::min(size - written, expected - within);
        std::copy_n(cachedBytes_.data() + within, count, output.data() + written);
        offset += count;
        written += count;
    }
    return output;
}
