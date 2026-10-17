#include "disc_import.h"
#include "dol_image.h"
#include "sha1.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <stdexcept>
#include <utility>

namespace {

uint32_t Read32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("GameCube metadata is truncated.");
    }
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

constexpr std::array<const char*, 3> kRelNames = {
    "wlp0.rel", "wlp1.rel", "wlp2.rel"
};
constexpr size_t kMaximumExecutableSize = 64u * 1024u * 1024u;

std::vector<uint8_t> ReadDiscHeader(RvzImage& image) {
    const auto header = image.Read(0, 0x440);
    if (Read32(header, 0x1c) != 0xc2339f3du) {
        throw std::runtime_error("Disc does not contain the GameCube magic word.");
    }
    return header;
}

std::vector<uint8_t> ReadFst(RvzImage& image, const std::vector<uint8_t>& header) {
    const uint32_t fstOffset = Read32(header, 0x424);
    const uint32_t fstSize = Read32(header, 0x428);
    if (fstOffset < header.size() ||
        fstSize < 12 || fstSize > kMaximumExecutableSize) {
        throw std::runtime_error("GameCube DOL/FST header offsets or sizes are invalid.");
    }
    return image.Read(fstOffset, fstSize);
}

std::vector<DiscFileEntry> ParseFst(const std::vector<uint8_t>& fst, uint64_t discSize) {
    const uint32_t count = Read32(fst, 8);
    if (Read32(fst, 0) != 0x01000000u || Read32(fst, 4) != 0 ||
        count == 0 || count > fst.size() / 12u) {
        throw std::runtime_error("GameCube FST root entry is invalid.");
    }
    struct Directory {
        uint32_t index;
        uint32_t end;
        std::string path;
    };
    std::vector<Directory> directories = {{0, count, ""}};
    std::vector<DiscFileEntry> entries;
    std::vector<std::string> paths;
    for (uint32_t i = 1; i < count; ++i) {
        while (directories.size() > 1 && i == directories.back().end) {
            directories.pop_back();
        }
        const uint32_t typeName = Read32(fst, i * 12u);
        const uint32_t type = typeName >> 24;
        const size_t nameOffset = static_cast<size_t>(count) * 12u +
                                  (typeName & 0x00ffffffu);
        if (type > 1 || nameOffset >= fst.size()) {
            throw std::runtime_error("GameCube FST entry type or name offset is invalid.");
        }
        const auto nameBegin = fst.begin() + nameOffset;
        const auto nameEnd = std::find(nameBegin, fst.end(), 0);
        if (nameEnd == fst.end()) {
            throw std::runtime_error("GameCube FST name is not null-terminated.");
        }
        const std::string name(nameBegin, nameEnd);
        if (name.empty() || name == "." || name == ".." ||
            name.find_first_of("/\\:") != std::string::npos) {
            throw std::runtime_error("GameCube FST contains an invalid filename.");
        }
        const uint32_t offset = Read32(fst, i * 12u + 4);
        const uint32_t size = Read32(fst, i * 12u + 8);
        const std::string path = directories.back().path + name;
        paths.push_back(path);
        if (type == 1) {
            if (offset != directories.back().index || size <= i ||
                size > directories.back().end) {
                throw std::runtime_error("GameCube FST directory hierarchy is invalid.");
            }
            directories.push_back({i, size, path + "/"});
        } else {
            if (offset > discSize || size > discSize - offset) {
                throw std::runtime_error("GameCube FST file extends beyond the disc.");
            }
            entries.push_back({path, offset, size});
        }
    }
    std::sort(paths.begin(), paths.end());
    if (std::adjacent_find(paths.begin(), paths.end()) != paths.end()) {
        throw std::runtime_error("GameCube FST contains a duplicated path.");
    }
    return entries;
}

} // namespace

std::vector<DiscFileEntry> ReadGameCubeFileTable(RvzImage& image) {
    return ParseFst(ReadFst(image, ReadDiscHeader(image)), image.metadata().discSize);
}

DiscFile ReadGameCubeFile(RvzImage& image, const std::string& path) {
    const auto entries = ReadGameCubeFileTable(image);
    const auto found = std::find_if(entries.begin(), entries.end(),
        [&path](const DiscFileEntry& entry) { return entry.path == path; });
    if (found == entries.end()) {
        throw std::runtime_error("GameCube disc file was not found: " + path);
    }
    return {path, image.Read(found->offset, found->size)};
}

std::vector<DiscFile> ReadGameCubeExecutables(RvzImage& image) {
    const auto header = ReadDiscHeader(image);
    const uint32_t dolOffset = Read32(header, 0x420);
    if (dolOffset < header.size()) {
        throw std::runtime_error("GameCube DOL/FST header offsets or sizes are invalid.");
    }
    const auto dolHeader = image.Read(dolOffset, 0x100);
    uint64_t dolSize = 0x100;
    for (size_t i = 0; i < 18; ++i) {
        const uint32_t offset = Read32(dolHeader, i * 4);
        const uint32_t size = Read32(dolHeader, 0x90 + i * 4);
        if (size != 0) {
            dolSize = std::max(dolSize, static_cast<uint64_t>(offset) + size);
        }
    }
    if (dolSize > kMaximumExecutableSize) {
        throw std::runtime_error("GameCube DOL exceeds the supported size limit.");
    }
    auto dol = image.Read(dolOffset, static_cast<size_t>(dolSize));
    const auto parsedDol = DolImage::Parse(dol);
    parsedDol.ReadWords(parsedDol.entryPoint(), 4);
    auto fst = ReadFst(image, header);
    const auto entries = ParseFst(fst, image.metadata().discSize);
    std::vector<DiscFile> files;
    files.push_back({"main.dol", std::move(dol)});
    files.push_back({"fst.bin", std::move(fst)});
    for (const char* name : kRelNames) {
        const DiscFileEntry* entry = nullptr;
        for (const auto& file : entries) {
            const auto separator = file.path.find_last_of('/');
            if (file.path.substr(separator == std::string::npos ? 0 : separator + 1) == name) {
                if (entry != nullptr || file.size == 0 || file.size > kMaximumExecutableSize) {
                    throw std::runtime_error(std::string("Required REL is empty, oversized or duplicated: ") + name);
                }
                entry = &file;
            }
        }
        if (entry == nullptr) {
            throw std::runtime_error(std::string("Required REL is absent from the FST: ") + name);
        }
        files.push_back({name, image.Read(entry->offset, entry->size)});
    }
    return files;
}

void VerifyUsExecutables(const std::vector<DiscFile>& files) {
    constexpr std::array<std::pair<const char*, const char*>, 4> expected = {{
        {"main.dol", "1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374"},
        {"wlp0.rel", "cf82697ee0d50ce11602b2b531e844f3d68c7318"},
        {"wlp1.rel", "509f77d903c89472be5590d45ddf65ddb2e93055"},
        {"wlp2.rel", "a160e6651fd50af8471a487b9a187044f683427c"}
    }};
    for (const auto& reference : expected) {
        const auto file = std::find_if(files.begin(), files.end(),
            [&reference](const DiscFile& value) { return value.name == reference.first; });
        if (file == files.end()) {
            throw std::runtime_error(std::string("Missing executable: ") + reference.first);
        }
        const std::string hash = Sha1Hex(file->bytes);
        if (hash != reference.second) {
            throw std::runtime_error("US revision 0 SHA-1 mismatch for " + file->name +
                                     ": expected " + reference.second + ", got " + hash);
        }
    }
}

std::vector<DiscFile> ImportUsRvz(const std::filesystem::path& input,
                                 const std::filesystem::path& outputDirectory) {
    RvzImage image(input);
    const auto& metadata = image.metadata();
    if (metadata.discId != "GYWEE9" || metadata.discNumber != 0 ||
        metadata.discVersion != 0) {
        throw std::runtime_error("Importer requires GYWEE9 disc 0, revision 0.");
    }
    auto files = ReadGameCubeExecutables(image);
    VerifyUsExecutables(files);
    if (!std::filesystem::create_directories(outputDirectory)) {
        throw std::runtime_error("Output directory already exists; choose a new directory.");
    }
    for (const auto& file : files) {
        const auto path = outputDirectory / file.name;
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("Could not create extracted file: " + path.string());
        }
        output.write(reinterpret_cast<const char*>(file.bytes.data()),
                     static_cast<std::streamsize>(file.bytes.size()));
        output.close();
        if (!output) {
            throw std::runtime_error("Could not finish writing extracted file: " + path.string());
        }
    }
    return files;
}
