#include "rel_image.h"
#include "disc_import.h"
#include "dol_image.h"
#include "sha1.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void Reject(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(message) != std::string::npos,
                "Unexpected REL rejection: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Invalid REL input was accepted: " + message);
}

uint32_t Read32(const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes.at(offset)) << 24) |
           (static_cast<uint32_t>(bytes.at(offset + 1)) << 16) |
           (static_cast<uint32_t>(bytes.at(offset + 2)) << 8) |
           bytes.at(offset + 3);
}

void Write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes.at(offset + i) = static_cast<uint8_t>(value >> (24u - i * 8u));
    }
}

struct Record {
    uint16_t delta;
    uint8_t type;
    uint8_t section;
    uint32_t addend;
};
using Import = std::pair<uint32_t, std::vector<Record>>;

std::vector<uint8_t> MakeRel(uint32_t id, const std::vector<Import>& imports) {
    std::vector<uint8_t> bytes(0xc0 + imports.size() * 8u, 0);
    Write32(bytes, 0, id);
    Write32(bytes, 0x0c, 4);
    Write32(bytes, 0x10, 0x4c);
    Write32(bytes, 0x1c, 3);
    Write32(bytes, 0x20, 0x10);
    Write32(bytes, 0x24, static_cast<uint32_t>(bytes.size()));
    Write32(bytes, 0x28, 0xc0);
    Write32(bytes, 0x2c, static_cast<uint32_t>(imports.size() * 8));
    bytes[0x30] = 1;
    bytes[0x31] = 1;
    bytes[0x32] = 1;
    Write32(bytes, 0x38, 4);
    Write32(bytes, 0x3c, 8);
    Write32(bytes, 0x40, 8);
    Write32(bytes, 0x44, 32);
    Write32(bytes, 0x54, 0x81);
    Write32(bytes, 0x58, 0x20);
    Write32(bytes, 0x5c, 0xa0);
    Write32(bytes, 0x60, 0x20);
    Write32(bytes, 0x68, 0x10);
    for (size_t i = 0x80; i < 0xa0; i += 4) {
        Write32(bytes, i, 0x48000001u);
    }
    std::fill(bytes.begin() + 0xa0, bytes.begin() + 0xc0, 0xa5);
    for (size_t i = 0; i < imports.size(); ++i) {
        Write32(bytes, 0xc0 + i * 8, imports[i].first);
        Write32(bytes, 0xc4 + i * 8, static_cast<uint32_t>(bytes.size()));
        for (const auto& record : imports[i].second) {
            const size_t offset = bytes.size();
            bytes.resize(offset + 8);
            bytes[offset] = static_cast<uint8_t>(record.delta >> 8);
            bytes[offset + 1] = static_cast<uint8_t>(record.delta);
            bytes[offset + 2] = record.type;
            bytes[offset + 3] = record.section;
            Write32(bytes, offset + 4, record.addend);
        }
    }
    Write32(bytes, 0x48, static_cast<uint32_t>(bytes.size()));
    return bytes;
}

std::vector<Import> MainImports() {
    return {
        {1, {{0, 202, 1, 0}, {0, 1, 2, 4}, {4, 4, 2, 4},
             {2, 5, 2, 4}, {2, 6, 3, 0}, {4, 10, 1, 16},
             {0, 202, 3, 0}, {0, 1, 2, 16}, {0, 203, 0, 0}}},
        {2, {{0, 202, 2, 0}, {0, 1, 1, 0}, {0, 203, 0, 0}}},
        {0, {{0, 202, 2, 0}, {4, 1, 0, 0x80348000u},
             {4, 5, 0, 0x80348000u}, {2, 6, 0, 0x80348000u},
             {2, 4, 0, 0x80348000u}, {0, 202, 1, 0},
             {16, 10, 0, 0x80003100u}, {0, 203, 0, 0}}}
    };
}

PpcState MakeState() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0x5a);
    return state;
}

void TestLinkAndRelocations() {
    const auto bytes = MakeRel(1, MainImports());
    const auto first = RelImage::Parse(bytes);
    const auto second = RelImage::Parse(MakeRel(2, {}));
    auto state = MakeState();
    const auto modules = LinkRelModules(state, {{&second, 0x80900000u}, {&first, 0x80800000u}},
                                        {{0x80000000u, 0x10000}});
    const auto& module = modules.at(1);
    const uint32_t text = module.sectionAddresses.at(1);
    const uint32_t data = module.sectionAddresses.at(2);
    const uint32_t bss = module.sectionAddresses.at(3);
    Require(module.id == 1 && module.prolog == text && module.epilog == text + 4 &&
                module.unresolved == text + 8 && bss % 32 == 0 &&
                text == 0x80800080u && data == 0x808000a0u,
            "REL section layout, entrypoints or BSS alignment is incorrect.");
    Require(ppc_read_u32(state, text) == data + 4 &&
                ppc_read_u16(state, text + 4) == ((data + 4) & 0xffffu) &&
                ppc_read_u16(state, text + 6) == (data >> 16) &&
                ppc_read_u16(state, text + 8) == ((bss + 0x8000u) >> 16),
            "Self ADDR32/LO/HI/HA relocations failed.");
    Require(ppc_read_u32(state, text + 12) == 0x48000005u,
            "REL24 did not preserve the branch opcode/link bit.");
    const uint32_t mainBranch = ppc_read_u32(state, text + 16);
    const uint32_t encodedDelta = mainBranch & 0x03fffffcu;
    const int64_t signedDelta = encodedDelta < 0x02000000u
        ? static_cast<int64_t>(encodedDelta) : static_cast<int64_t>(encodedDelta) - 0x04000000ll;
    Require(static_cast<int64_t>(text + 16) + signedDelta == 0x80003100u &&
                (mainBranch & 0xfc000003u) == 0x48000001u,
            "Negative DOL REL24 fixup failed.");
    Require(ppc_read_u32(state, data) == 0x80900080u &&
                ppc_read_u32(state, data + 4) == 0x80348000u &&
                ppc_read_u16(state, data + 8) == 0x8034 &&
                ppc_read_u16(state, data + 10) == 0x8035 &&
                ppc_read_u16(state, data + 12) == 0x8000,
            "Cross-module or DOL imports / high-adjust rounding failed.");
    Require(ppc_read_u32(state, bss) == data + 16 &&
                ppc_read_u32(state, bss + 4) == 0 &&
                ppc_read_u32(state, bss + 12) == 0 &&
                ppc_read_u8(state, module.address - 1) == 0x5a &&
                ppc_read_u8(state, module.address + module.size) == 0x5a,
            "BSS fixup, zero fill or load guard failed.");
    Require(first.bytes() == bytes, "Linking modified the parsed source image.");
    Require(state.pc == 0 && !state.callGuest, "Linker attempted to execute a module.");
}

void TestInvalidInputs() {
    const auto good = MakeRel(1, MainImports());
    for (unsigned test = 0; test < 12; ++test) {
        auto bytes = good;
        switch (test) {
        case 0: bytes.resize(0x4b); break;
        case 1: Write32(bytes, 0x1c, 4); break;
        case 2: Write32(bytes, 0x0c, UINT32_MAX); break;
        case 3: Write32(bytes, 0x54, 0xfffffffd); break;
        case 4: Write32(bytes, 0x20, 32); break;
        case 5: Write32(bytes, 0x34, 0x20); break;
        case 6: Write32(bytes, 0x40, 3); break;
        case 7: Write32(bytes, 0xc8, 1); break;
        case 8: bytes[0xda] = 1; break; // replace first SECTION with ADDR32
        case 9: bytes[0xe2] = 99; break;
        case 10: bytes[0xe0] = 0xff; bytes[0xe1] = 0xff; break;
        case 11: Write32(bytes, 0x5c, 0x80); break;
        }
        const char* messages[] = {"truncated", "version", "section table", "file range",
            "BSS size", "entrypoint", "alignment", "duplicated", "before a section",
            "Unsupported", "offset exceeds", "overlap"};
        Reject([&] { RelImage::Parse(bytes); }, messages[test]);
    }
    auto imports = MainImports();
    imports[0].second.pop_back();
    Reject([&] { RelImage::Parse(MakeRel(1, imports)); }, "END marker");
    imports = MainImports();
    imports[0].second[1].delta = 2;
    Reject([&] { RelImage::Parse(MakeRel(1, imports)); }, "misaligned");
    imports = MainImports();
    imports[0].second[0].section = 4;
    Reject([&] { RelImage::Parse(MakeRel(1, imports)); }, "patch section");
}

void TestAtomicLinkFailures() {
    const auto first = RelImage::Parse(MakeRel(1, MainImports()));
    const auto second = RelImage::Parse(MakeRel(2, {}));
    auto state = MakeState();
    const auto before = state.memory;
    PpcState flat;
    Reject([&] { LinkRelModules(flat, {{&first, 0}}, {}); }, "GameCube RAM");
    Reject([&] { LinkRelModules(state, {{nullptr, 0x80800000u}}, {}); }, "no parsed image");
    Reject([&] { LinkRelModules(state, {{&first, 0x80800000u}}, {}); }, "missing module 2");
    Reject([&] { LinkRelModules(state, {{&first, 0x80800001u}}, {}); }, "misaligned");
    Reject([&] { LinkRelModules(state, {{&first, 0x817fff00u}}, {}); }, "boundary");
    Reject([&] { LinkRelModules(state, {{&first, 0x80800000u}, {&first, 0x80900000u}}, {}); },
           "duplicate module ID");
    Reject([&] { LinkRelModules(state, {{&first, 0x80800000u}, {&second, 0xc0800000u}}, {}); },
           "overlaps");
    Reject([&] { LinkRelModules(state, {{&first, 0x80800000u}},
                               {{0xc0800000u, 0x1000}}); }, "overlaps");
    auto imports = MainImports();
    imports[1].second[1].section = 9;
    auto badTarget = RelImage::Parse(MakeRel(1, imports));
    Reject([&] { LinkRelModules(state, {{&badTarget, 0x80800000u},
                                      {&second, 0x80900000u}}, {}); }, "target section");
    imports = MainImports();
    imports[1].second[1].addend = 0x100;
    auto badAddend = RelImage::Parse(MakeRel(1, imports));
    Reject([&] { LinkRelModules(state, {{&badAddend, 0x80800000u},
                                      {&second, 0x80900000u}}, {}); }, "target section");
    imports = MainImports();
    imports[2].second.back() = {0, 203, 0, 0};
    imports[2].second[6].addend = 0x90000000u;
    auto farBranch = RelImage::Parse(MakeRel(1, imports));
    Reject([&] { LinkRelModules(state, {{&farBranch, 0x80800000u},
                                      {&second, 0x80900000u}}, {}); }, "REL24");
    auto invalidBranchBytes = MakeRel(1, MainImports());
    Write32(invalidBranchBytes, 0x8c, 0x60000000u);
    auto invalidBranch = RelImage::Parse(invalidBranchBytes);
    Reject([&] { LinkRelModules(state, {{&invalidBranch, 0x80800000u},
                                      {&second, 0x80900000u}}, {}); }, "REL24");
    Require(state.memory == before, "A failed REL link partially changed guest memory.");
}

void TestNopDeltaAndBoundary() {
    auto bytes = MakeRel(1, {{1, {{0, 202, 1, 0}, {4, 201, 0, 0},
                                 {4, 0, 0, 0}, {4, 1, 2, 0}, {0, 203, 0, 0}}}});
    auto image = RelImage::Parse(bytes);
    auto state = MakeState();
    const auto modules = LinkRelModules(state, {{&image, 0x80800000u}}, {});
    Require(ppc_read_u32(state, 0x8080008cu) == 0x808000a0u &&
                ppc_read_u32(state, 0x80800080u) == 0x48000001u,
            "REL NONE/NOP did not advance the cumulative patch offset.");
    for (uint32_t addend : {0x7ffffffeu, 0x90000000u}) {
        auto branch = RelImage::Parse(MakeRel(1, {{0, {{0, 202, 1, 0},
            {0, 10, 0, addend}, {0, 203, 0, 0}}}}));
        Reject([&] { LinkRelModules(state, {{&branch, 0x80800000u}}, {}); }, "REL24");
    }
}

void TestBranchLimitsAndLongDelta() {
    auto state = MakeState();
    constexpr uint32_t patchAddress = 0x80800080u;
    for (int64_t delta : {-0x2000000ll, -4ll, 0ll, 4ll, 0x1fffffcll}) {
        const uint32_t target = static_cast<uint32_t>(
            static_cast<int64_t>(patchAddress) + delta);
        auto image = RelImage::Parse(MakeRel(1, {{0, {{0, 202, 1, 0},
            {0, 10, 0, target}, {0, 203, 0, 0}}}}));
        LinkRelModules(state, {{&image, 0x80800000u}}, {});
        Require(ppc_read_u32(state, patchAddress) ==
                    (0x48000001u | (static_cast<uint32_t>(delta) & 0x03fffffcu)),
                "REL24 rejected or misencoded an inclusive displacement boundary.");
    }
    for (int64_t delta : {-0x2000004ll, 0x2000000ll}) {
        const uint32_t target = static_cast<uint32_t>(
            static_cast<int64_t>(patchAddress) + delta);
        auto image = RelImage::Parse(MakeRel(1, {{0, {{0, 202, 1, 0},
            {0, 10, 0, target}, {0, 203, 0, 0}}}}));
        Reject([&] { LinkRelModules(state, {{&image, 0x80800000u}}, {}); }, "REL24");
    }
    for (uint32_t target : {0x12347fffu, 0x12348000u, 0xffff8000u, 0xffffffffu}) {
        auto image = RelImage::Parse(MakeRel(1, {{0, {{0, 202, 2, 0},
            {0, 6, 0, target}, {0, 203, 0, 0}}}}));
        LinkRelModules(state, {{&image, 0x80800000u}}, {});
        Require(ppc_read_u16(state, 0x808000a0u) ==
                    (((static_cast<uint64_t>(target) + 32768u) / 65536u) & 0xffffu),
                "ADDR16_HA mishandled the sign boundary or 32-bit wraparound.");
    }
    auto bytes = MakeRel(1, {{0, {{0, 202, 2, 0},
        {0xffffu, 201, 0, 0}, {1, 1, 0, 0x80348000u}, {0, 203, 0, 0}}}});
    const size_t metadataSize = bytes.size() - 0xc0u;
    bytes.insert(bytes.begin() + 0xc0, 0x10000, 0);
    Write32(bytes, 0x60, 0x10020u);
    Write32(bytes, 0x24, Read32(bytes, 0x24) + 0x10000u);
    Write32(bytes, 0x28, Read32(bytes, 0x28) + 0x10000u);
    Write32(bytes, 0x100c4, Read32(bytes, 0x100c4) + 0x10000u);
    Write32(bytes, 0x48, static_cast<uint32_t>(bytes.size()));
    Require(bytes.size() == 0x100c0u + metadataSize, "Long-delta fixture is malformed.");
    auto image = RelImage::Parse(bytes);
    LinkRelModules(state, {{&image, 0x80800000u}}, {});
    Require(ppc_read_u32(state, 0x808100a0u) == 0x80348000u,
            "NOP relocation did not extend the patch delta beyond 65535 bytes.");
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Could not open US module input: " + path.string());
    }
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), {});
}

void TestOriginalModules(const std::filesystem::path& directory) {
    std::vector<DiscFile> files;
    for (const char* name : {"main.dol", "wlp0.rel", "wlp1.rel", "wlp2.rel"}) {
        files.push_back({name, ReadFile(directory / name)});
    }
    VerifyUsExecutables(files);
    const auto dol = DolImage::Parse(files[0].bytes);
    auto state = dol.CreateGameCubeState();
    std::vector<RelMemoryRange> reserved = {{0x80000000u, 0x3100},
                                           {0x8034cfa0u, 0x10000}};
    for (const auto& section : dol.sections()) {
        reserved.push_back({section.address, section.size});
    }
    if (dol.bssSize()) {
        reserved.push_back({dol.bssAddress(), dol.bssSize()});
    }
    std::vector<RelImage> images;
    for (size_t i = 1; i < files.size(); ++i) {
        images.push_back(RelImage::Parse(files[i].bytes));
    }
    const auto linked = LinkRelModules(state,
        {{&images[0], 0x81000000u}, {&images[1], 0x81200000u}, {&images[2], 0x81400000u}},
        reserved);
    constexpr size_t expectedCounts[] = {9715, 15170, 7752};
    size_t verifiedPatches = 0;
    for (size_t i = 0; i < linked.size(); ++i) {
        const auto& module = linked[i];
        Require(module.id == i + 1 && module.relocationCount == expectedCounts[i] &&
                    module.prolog != 0, "Unexpected original REL ID/fixup count/prolog.");
        for (const auto& fixup : images[i].relocations()) {
                    uint32_t target = fixup.addend;
                    if (fixup.importId != 0) {
                        const auto dependency = std::find_if(linked.begin(), linked.end(),
                            [&fixup](const LinkedRelModule& value) { return value.id == fixup.importId; });
                        Require(dependency != linked.end(), "Original dependency was not linked.");
                        target += dependency->sectionAddresses.at(fixup.targetSection);
                    }
                    const uint32_t patch = module.sectionAddresses.at(fixup.patchSection) +
                                           fixup.patchOffset;
                    if (fixup.type == 1) {
                        Require(ppc_read_u32(state, patch) == target, "Original ADDR32 mismatch.");
                    } else if (fixup.type == 4 || fixup.type == 6) {
                        const uint32_t expected = fixup.type == 4 ? target & 0xffffu :
                            ((target >> 16) + ((target & 0x8000u) != 0 ? 1u : 0u)) & 0xffffu;
                        Require(ppc_read_u16(state, patch) == expected, "Original LO/HA mismatch.");
                    } else if (fixup.type == 10) {
                        const uint32_t instruction = ppc_read_u32(state, patch);
                        const uint32_t bits = instruction & 0x03fffffcu;
                        const int64_t displacement = bits < 0x02000000u ? int64_t{bits} :
                                                     int64_t{bits} - 0x04000000ll;
                        const auto& section = images[i].sections().at(fixup.patchSection);
                        const uint32_t original = Read32(images[i].bytes(),
                                                         section.fileOffset + fixup.patchOffset);
                        Require(static_cast<int64_t>(patch) + displacement == target &&
                                    (instruction & 0xfc000003u) == (original & 0xfc000003u),
                                "Original REL24 target or opcode/link flags mismatch.");
                    } else {
                        throw std::runtime_error("Unexpected relocation type in the hash-verified originals.");
                    }
                    ++verifiedPatches;
        }
        for (size_t section = 0; section < images[i].sections().size(); ++section) {
            const auto& info = images[i].sections()[section];
            if (info.size != 0 && info.fileOffset == 0) {
                const size_t offset = ppc_memory_offset(state, module.sectionAddresses[section], info.size);
                Require(std::all_of(state.memory.begin() + offset,
                                    state.memory.begin() + offset + info.size,
                                    [](uint8_t b) { return b == 0; }),
                        "Original REL BSS was not zero initialized.");
            }
        }
        std::cout << files[i + 1].name << ": module " << module.id << ", "
                  << module.relocationCount << " fixups, prolog 0x" << std::hex
                  << module.prolog << std::dec << '\n';
    }
    // Independent samples from the original executable's first external call sites.
    Require(ppc_read_u16(state, 0x8100010au) == 0x8035u &&
                ppc_read_u16(state, 0x8100011eu) == 0x91c4u,
            "Original wlp0 DOL data import was not relocated correctly.");
    Require(ppc_read_u32(state, 0x8100012cu) ==
                (0x48000001u | ((0x801020e4u - 0x8100012cu) & 0x03fffffcu)),
            "Original wlp0 DOL function call was not relocated correctly.");
    for (const auto& section : dol.sections()) {
        if (section.executable) {
            const auto words = dol.ReadWords(section.address, section.size);
            for (size_t i = 0; i < words.size(); ++i) {
                Require(ppc_read_u32(state, section.address + static_cast<uint32_t>(i * 4)) ==
                            words[i], "REL linking modified original DOL text.");
            }
        }
    }
    Require(verifiedPatches == 32637, "Original fixup verification count is incorrect.");
    std::cout << verifiedPatches << " original relocation results verified; DOL text unchanged.\n";
    std::cout << "All three hash-verified US RELs linked in test RAM; "
                 "no module code executed and no OSLink/runtime integration implied.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--us") {
            TestOriginalModules(argv[2]);
        } else if (argc == 1) {
            TestLinkAndRelocations();
            TestInvalidInputs();
            TestAtomicLinkFailures();
            TestNopDeltaAndBoundary();
            TestBranchLimitsAndLongDelta();
        } else {
            throw std::runtime_error("Usage: hmawl_rel_tests [--us <verified-game-directory>]");
        }
    } catch (const std::exception& error) {
        std::cerr << "REL test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All REL module tests passed.\n";
    return 0;
}
