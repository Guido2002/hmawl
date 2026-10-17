#include "rel_image.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

uint32_t Read32(const std::vector<uint8_t>& bytes, size_t offset) {
    if (offset > bytes.size() || bytes.size() - offset < 4) {
        throw std::runtime_error("REL word read extends beyond the file.");
    }
    return (static_cast<uint32_t>(bytes[offset]) << 24) |
           (static_cast<uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<uint32_t>(bytes[offset + 2]) << 8) | bytes[offset + 3];
}

void Write16(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    bytes.at(offset) = static_cast<uint8_t>(value >> 8);
    bytes.at(offset + 1) = static_cast<uint8_t>(value);
}

void Write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    Write16(bytes, offset, value >> 16);
    Write16(bytes, offset + 2, value);
}

bool IsPowerOfTwo(uint32_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

uint64_t Align(uint64_t value, uint32_t alignment) {
    return (value + alignment - 1u) & ~(static_cast<uint64_t>(alignment) - 1u);
}

size_t PatchSize(uint8_t type) {
    switch (type) {
    case 1: // R_PPC_ADDR32
    case 10: // R_PPC_REL24
        return 4;
    case 4: // R_PPC_ADDR16_LO
    case 5: // R_PPC_ADDR16_HI
    case 6: // R_PPC_ADDR16_HA
        return 2;
    default:
        throw std::runtime_error("Unsupported REL relocation type " +
                                 std::to_string(type) + ".");
    }
}

} // namespace

RelImage RelImage::Parse(std::vector<uint8_t> bytes) {
    if (bytes.size() < 0x4c || bytes.size() > 64u * 1024u * 1024u) {
        throw std::runtime_error("REL file is truncated or exceeds the size limit.");
    }
    if (Read32(bytes, 0x1c) != 3) {
        throw std::runtime_error("Only REL version 3 is supported.");
    }
    RelImage image;
    image.id_ = Read32(bytes, 0);
    image.alignment_ = Read32(bytes, 0x40);
    image.bssAlignment_ = Read32(bytes, 0x44);
    if (image.id_ == 0 || !IsPowerOfTwo(image.alignment_) ||
        !IsPowerOfTwo(image.bssAlignment_) ||
        image.alignment_ > kGameCubeRamSize || image.bssAlignment_ > kGameCubeRamSize) {
        throw std::runtime_error("REL module ID or alignment is invalid.");
    }
    const uint32_t count = Read32(bytes, 0x0c);
    const uint32_t table = Read32(bytes, 0x10);
    if (count == 0 || count > 256 || table < 0x4c ||
        (table & 3u) != 0 || table > bytes.size() ||
        count > (bytes.size() - table) / 8u) {
        throw std::runtime_error("REL section table is invalid.");
    }
    const size_t sectionTableEnd = table + static_cast<size_t>(count) * 8u;
    uint32_t bssSize = 0;
    bool foundBss = false;
    std::vector<std::pair<size_t, size_t>> ranges;
    for (uint32_t index = 0; index < count; ++index) {
        const uint32_t flags = Read32(bytes, table + index * 8u);
        const uint32_t offset = flags & ~1u;
        const uint32_t size = Read32(bytes, table + index * 8u + 4u);
        const bool executable = (flags & 1u) != 0;
        if (index == 0 && (flags != 0 || size != 0)) {
            throw std::runtime_error("REL section zero must be empty.");
        }
        if (size != 0) {
            if (offset == 0) {
                if (executable || foundBss) {
                    throw std::runtime_error("REL has an executable or duplicate BSS section.");
                }
                foundBss = true;
                bssSize = size;
            } else {
                if ((offset & 3u) != 0 || offset < sectionTableEnd ||
                    offset > bytes.size() || size > bytes.size() - offset ||
                    (executable && (size & 3u) != 0)) {
                    throw std::runtime_error("REL section has an invalid file range.");
                }
                for (const auto& range : ranges) {
                    if (offset < range.second && range.first < static_cast<uint64_t>(offset) + size) {
                        throw std::runtime_error("REL file sections overlap.");
                    }
                }
                ranges.push_back({offset, static_cast<size_t>(offset) + size});
            }
        }
        image.sections_.push_back({offset, size, executable});
    }
    if (bssSize != Read32(bytes, 0x20) || bssSize > kGameCubeRamSize) {
        throw std::runtime_error("REL BSS size does not match the section table.");
    }
    for (size_t i = 0; i < 3; ++i) {
        const uint8_t section = bytes[0x30 + i];
        const uint32_t offset = Read32(bytes, 0x34 + i * 4);
        if (section == 0) {
            if (offset != 0) {
                throw std::runtime_error("REL absent entrypoint has a nonzero offset.");
            }
        } else if (section >= image.sections_.size() ||
                   !image.sections_[section].executable ||
                   (offset & 3u) != 0 ||
                   image.sections_[section].size < 4 ||
                   offset > image.sections_[section].size - 4) {
            throw std::runtime_error("REL entrypoint is outside an executable section.");
        }
        image.entrySections_[i] = section;
        image.entryOffsets_[i] = offset;
    }
    const uint32_t importOffset = Read32(bytes, 0x28);
    const uint32_t importSize = Read32(bytes, 0x2c);
    const uint32_t relocOffset = Read32(bytes, 0x24);
    if (importOffset < sectionTableEnd || (importOffset & 3u) != 0 ||
        (importSize & 7u) != 0 || importOffset > bytes.size() ||
        importSize > bytes.size() - importOffset ||
        relocOffset < static_cast<uint64_t>(importOffset) + importSize ||
        (relocOffset & 3u) != 0 || relocOffset > bytes.size()) {
        throw std::runtime_error("REL import/relocation table range is invalid.");
    }
    for (const auto& range : ranges) {
        if (range.second > importOffset) {
            throw std::runtime_error("REL section overlaps import/relocation metadata.");
        }
    }
    std::vector<uint32_t> streams;
    for (uint32_t i = 0; i < importSize; i += 8) {
        const uint32_t id = Read32(bytes, importOffset + i);
        const uint32_t offset = Read32(bytes, importOffset + i + 4);
        if (std::find(image.imports_.begin(), image.imports_.end(), id) != image.imports_.end() ||
            offset < relocOffset || (offset & 3u) != 0 ||
            offset > bytes.size() || bytes.size() - offset < 8) {
            throw std::runtime_error("REL import ID is duplicated or its stream is invalid.");
        }
        image.imports_.push_back(id);
        streams.push_back(offset);
    }
    for (size_t i = 0; i < streams.size(); ++i) {
        size_t limit = bytes.size();
        for (size_t j = 0; j < streams.size(); ++j) {
            if (i != j && streams[i] == streams[j]) {
                throw std::runtime_error("REL import streams overlap.");
            }
            if (streams[j] > streams[i]) {
                limit = std::min(limit, static_cast<size_t>(streams[j]));
            }
        }
        int patchSection = -1;
        uint64_t patchOffset = 0;
        bool ended = false;
        for (size_t offset = streams[i]; offset <= limit && limit - offset >= 8; offset += 8) {
            const uint16_t delta = static_cast<uint16_t>(
                (static_cast<uint16_t>(bytes[offset]) << 8) | bytes[offset + 1]);
            const uint8_t type = bytes[offset + 2];
            const uint8_t targetSection = bytes[offset + 3];
            const uint32_t addend = Read32(bytes, offset + 4);
            if (type == 203) { // R_DOLPHIN_END
                ended = true;
                break;
            }
            if (type == 202) { // R_DOLPHIN_SECTION
                if (targetSection >= image.sections_.size() ||
                    image.sections_[targetSection].size == 0) {
                    throw std::runtime_error("REL patch section is invalid.");
                }
                patchSection = targetSection;
                patchOffset = 0;
                continue;
            }
            if (patchSection < 0) {
                throw std::runtime_error("REL relocation occurs before a section marker.");
            }
            patchOffset += delta;
            const auto& section = image.sections_[static_cast<size_t>(patchSection)];
            if (patchOffset > section.size) {
                throw std::runtime_error("REL relocation offset exceeds its patch section.");
            }
            if (type == 0 || type == 201) { // NONE / R_DOLPHIN_NOP still advances the offset
                continue;
            }
            const size_t width = PatchSize(type);
            if (width > section.size - patchOffset || patchOffset % width != 0) {
                throw std::runtime_error("REL patch is misaligned or crosses its section boundary.");
            }
            image.relocations_.push_back({
                image.imports_[i], static_cast<uint8_t>(patchSection),
                static_cast<uint32_t>(patchOffset), type, targetSection, addend});
        }
        if (!ended) {
            throw std::runtime_error("REL relocation stream is missing its END marker.");
        }
    }
    image.bytes_ = std::move(bytes);
    return image;
}

std::vector<LinkedRelModule> LinkRelModules(
    PpcState& state, const std::vector<RelPlacement>& placements,
    const std::vector<RelMemoryRange>& reserved) {
    if (state.memoryMap != PpcMemoryMap::GameCubeRam) {
        throw std::runtime_error("REL linking requires the GameCube RAM memory map.");
    }
    struct StagedModule {
        const RelImage* image;
        LinkedRelModule linked;
        std::vector<uint8_t> bytes;
        size_t ramOffset;
    };
    std::vector<std::pair<size_t, size_t>> occupied;
    for (const auto& range : reserved) {
        if (range.size == 0) {
            continue;
        }
        const size_t offset = ppc_memory_offset(state, range.address, range.size);
        occupied.push_back({offset, offset + range.size});
    }
    std::vector<StagedModule> staged;
    std::map<uint32_t, size_t> ids;
    for (const auto& placement : placements) {
        if (!placement.image || placement.image->sections().empty() ||
            ids.find(placement.image->id()) != ids.end()) {
            throw std::runtime_error("REL placement has no parsed image or a duplicate module ID.");
        }
        const auto& image = *placement.image;
        if (placement.address % image.alignment() != 0) {
            throw std::runtime_error("REL module load address is misaligned.");
        }
        uint64_t end = static_cast<uint64_t>(placement.address) + image.bytes().size();
        uint64_t bssAddress = 0;
        for (const auto& section : image.sections()) {
            if (section.size != 0 && section.fileOffset == 0) {
                bssAddress = Align(end, image.bssAlignment());
                end = bssAddress + section.size;
            }
        }
        if (end > std::numeric_limits<uint32_t>::max() ||
            end - placement.address > kGameCubeRamSize) {
            throw std::runtime_error("REL loaded address range overflows.");
        }
        const uint32_t size = static_cast<uint32_t>(end - placement.address);
        const size_t ramOffset = ppc_memory_offset(state, placement.address, size);
        for (const auto& range : occupied) {
            if (ramOffset < range.second && range.first < ramOffset + size) {
                throw std::runtime_error("REL load range overlaps reserved memory or another module.");
            }
        }
        occupied.push_back({ramOffset, ramOffset + size});
        LinkedRelModule linked = {image.id(), placement.address, size, {}, 0, 0, 0,
                                  image.relocations().size()};
        for (const auto& section : image.sections()) {
            linked.sectionAddresses.push_back(section.size == 0 ? 0u :
                section.fileOffset == 0 ? static_cast<uint32_t>(bssAddress) :
                                         placement.address + section.fileOffset);
        }
        uint32_t* entries[] = {&linked.prolog, &linked.epilog, &linked.unresolved};
        for (size_t i = 0; i < 3; ++i) {
            if (image.entrySections()[i] != 0) {
                *entries[i] = linked.sectionAddresses[image.entrySections()[i]] +
                              image.entryOffsets()[i];
            }
        }
        std::vector<uint8_t> bytes(size, 0);
        std::copy(image.bytes().begin(), image.bytes().end(), bytes.begin());
        ids.emplace(image.id(), staged.size());
        staged.push_back({&image, std::move(linked), std::move(bytes), ramOffset});
    }
    for (auto& module : staged) {
        for (uint32_t import : module.image->imports()) {
            if (import != 0 && ids.find(import) == ids.end()) {
                throw std::runtime_error("REL module " + std::to_string(module.linked.id) +
                    " requires missing module " + std::to_string(import) + ".");
            }
        }
        for (const auto& reloc : module.image->relocations()) {
            uint64_t target = reloc.addend;
            if (reloc.importId != 0) {
                const auto& dependency = staged.at(ids.at(reloc.importId));
                if (reloc.targetSection >= dependency.image->sections().size() ||
                    dependency.linked.sectionAddresses[reloc.targetSection] == 0 ||
                    reloc.addend > dependency.image->sections()[reloc.targetSection].size ||
                    (reloc.type == 10 &&
                     reloc.addend == dependency.image->sections()[reloc.targetSection].size)) {
                    throw std::runtime_error("REL import references an invalid target section or offset.");
                }
                target += dependency.linked.sectionAddresses[reloc.targetSection];
            }
            if (target > std::numeric_limits<uint32_t>::max()) {
                throw std::runtime_error("REL target address overflows.");
            }
            const uint32_t value = static_cast<uint32_t>(target);
            const uint32_t patchAddress =
                module.linked.sectionAddresses[reloc.patchSection] + reloc.patchOffset;
            if (patchAddress % PatchSize(reloc.type) != 0) {
                throw std::runtime_error("REL loaded patch address is misaligned.");
            }
            const size_t patch = patchAddress - module.linked.address;
            switch (reloc.type) {
            case 1:
                Write32(module.bytes, patch, value);
                break;
            case 4:
                Write16(module.bytes, patch, value);
                break;
            case 5:
                Write16(module.bytes, patch, value >> 16);
                break;
            case 6:
                Write16(module.bytes, patch, static_cast<uint32_t>((target + 0x8000u) >> 16));
                break;
            case 10: {
                const int64_t displacement = static_cast<int64_t>(value) - patchAddress;
                const uint32_t instruction = Read32(module.bytes, patch);
                if ((value & 3u) != 0 || displacement < -0x2000000ll ||
                    displacement > 0x1fffffcll ||
                    (instruction & 0xfc000002u) != 0x48000000u) {
                    throw std::runtime_error("REL24 requires an aligned, in-range relative branch.");
                }
                Write32(module.bytes, patch, (instruction & 0xfc000003u) |
                        (static_cast<uint32_t>(displacement) & 0x03fffffcu));
                break;
            }
            default:
                throw std::runtime_error("Unsupported staged REL relocation.");
            }
        }
    }
    std::vector<LinkedRelModule> result;
    result.reserve(staged.size());
    for (auto& module : staged) {
        result.push_back(std::move(module.linked));
    }
    for (const auto& module : staged) {
        std::copy(module.bytes.begin(), module.bytes.end(),
                  state.memory.begin() + module.ramOffset);
    }
    return result;
}
