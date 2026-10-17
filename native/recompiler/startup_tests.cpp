#include "dol_image.h"
#include "sha1.h"
#include "generated_us_startup.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestEntryAndRegisters(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    std::fill(std::begin(state.gpr), std::end(state.gpr), 0xdeadbeefu);
    unsigned calls = 0;
    state.callGuest = [&calls](uint32_t address, PpcState& guest) {
        Require(address == 0x800032b0u, "Entry called an unexpected function.");
        ++calls;
        ppc_800032B0(guest);
        Require(guest.pc == guest.lr, "Register initialization did not return to entry.");
    };
    ppc_80003154(state);
    Require(calls == 1 && state.pc == 0x80003158u && state.lr == 0x80003158u,
            "First entry instruction did not call/return correctly.");
    Require(state.gpr[1] == 0x8035cfa0u && state.gpr[2] == 0x80351e40u &&
                state.gpr[13] == 0x8034e9c0u,
            "Original startup produced incorrect stack or SDA registers.");
    for (unsigned reg = 0; reg < 32; ++reg) {
        if (reg != 1 && reg != 2 && reg != 13) {
            Require(state.gpr[reg] == 0, "Original startup did not clear a register.");
        }
    }
    std::cout << "US entry -> register initialization passed; next PC 0x80003158 "
                 "(__init_hardware call, not executed).\n";
}

void TestStartupHelpers(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    state.lr = 0x80001000u;
    for (uint16_t buttons : {uint16_t{0}, uint16_t{0xeef}, uint16_t{0xffff}}) {
        ppc_write_u16(state, 0x800030e4u, buttons);
        const uint32_t stack = state.gpr[1];
        unsigned resets = 0;
        state.callGuest = [&resets](uint32_t address, PpcState& guest) {
            Require(address == 0x801b568cu && guest.gpr[3] == 0 &&
                        guest.gpr[4] == 0 && guest.gpr[5] == 0,
                    "Reset check called an unexpected service or passed wrong arguments.");
            ++resets;
        };
        ppc_80003100(state);
        Require(resets == static_cast<unsigned>((buttons & 0xeef) == 0xeef) &&
                    state.gpr[1] == stack && state.pc == 0x80001000u,
                "Original pad/reset helper failed or damaged its stack/return address.");
    }
    ppc_80003140(state);
    ppc_8000314C(state);
    Require(state.gpr[3] == 1 &&
                ppc_read_u8(state, state.gpr[13] - 0x5338u) == 1,
            "Original SDA flag setter/getter failed.");
    for (uint32_t count : {0u, 1u, 17u, 257u}) {
        for (uint32_t i = 0; i < count; ++i) {
            ppc_write_u8(state, 0x80001000u + i, static_cast<uint8_t>(i ^ 0xa5));
        }
        ppc_write_u8(state, 0x80002000u, 0xee);
        state.gpr[3] = 0x80002000u;
        state.gpr[4] = 0x80001000u;
        state.gpr[5] = count;
        ppc_80003488(state);
        for (uint32_t i = 0; i < count; ++i) {
            Require(ppc_read_u8(state, 0x80002000u + i) ==
                        static_cast<uint8_t>(i ^ 0xa5), "Original startup byte copy failed.");
        }
        Require(count != 0 || ppc_read_u8(state, 0x80002000u) == 0xee,
                "Zero-length byte copy wrote memory.");
    }
    std::cout << "US pad/reset helper (test callback only), SDA flag and byte-copy helpers passed.\n";
}

void TestArgumentRelocation(const DolImage& image) {
    for (unsigned path = 0; path < 4; ++path) {
        auto state = image.CreateGameCubeState();
        ppc_write_u32(state, 0x800000f4u, path == 0 ? 0u : 0x80001000u);
        ppc_write_u32(state, 0x80001008u, path < 2 ? 0u : 0x20u);
        ppc_write_u32(state, 0x80001020u, path < 3 ? 0u : 3u);
        for (uint32_t i = 0; i < 3; ++i) {
            ppc_write_u32(state, 0x80001024u + i * 4, 0x80u + i * 16u);
        }
        ppc_800031F8(state);
        Require(state.pc == 0x80003260u &&
                    state.gpr[14] == (path == 3 ? 3u : 0u) &&
                    state.gpr[15] == (path == 3 ? 0x80001024u : 0u),
                "Original argument setup selected an incorrect path or continuation.");
        if (path == 3) {
            Require(state.ctr == 0 && ppc_read_u32(state, 0x80000034u) == 0x80001020u,
                    "Original argument setup did not update CTR or arena boundary.");
            for (uint32_t i = 0; i < 3; ++i) {
                Require(ppc_read_u32(state, 0x80001024u + i * 4) ==
                            0x80001080u + i * 16u, "Argument pointer relocation failed.");
            }
        }
    }
    std::cout << "US argument-pointer relocation passed for absent, empty and populated arguments.\n";
}

void BindMemoryFunctions(PpcState& state) {
    state.callGuest = [](uint32_t address, PpcState& guest) {
        switch (address) {
        case 0x800055ccu:
            ppc_800055CC(guest);
            break;
        case 0x800054e4u:
            ppc_800054E4(guest);
            break;
        case 0x80005514u:
            ppc_80005514(guest);
            break;
        default:
            throw std::runtime_error("Unsupported startup memory dependency at " +
                                     std::to_string(address));
        }
    };
}

void TestMemoryFunctions(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    BindMemoryFunctions(state);
    const uint32_t stack = state.gpr[1];
    constexpr uint32_t base = 0x80001000u;
    for (uint32_t alignment = 0; alignment < 4; ++alignment) {
        for (uint32_t count : {0u, 1u, 3u, 4u, 7u, 31u, 32u, 33u, 63u, 64u, 65u, 257u}) {
            for (uint32_t fill : {0u, 0xa5u, 0x123456ffu}) {
                std::vector<uint8_t> expected(512, 0xcc);
                for (uint32_t i = 0; i < expected.size(); ++i) {
                    ppc_write_u8(state, base + i, expected[i]);
                }
                const uint32_t start = 8u + alignment;
                std::fill_n(expected.begin() + start, count, static_cast<uint8_t>(fill));
                state.gpr[3] = base + start;
                state.gpr[4] = fill;
                state.gpr[5] = count;
                state.gpr[31] = 0x98765432u;
                state.lr = 0x80001000u;
                ppc_800054E4(state);
                for (uint32_t i = 0; i < expected.size(); ++i) {
                    Require(ppc_read_u8(state, base + i) == expected[i],
                            "Original memset changed the wrong byte or overwrote its guard.");
                }
                Require(state.gpr[3] == base + start && state.gpr[1] == stack &&
                            state.gpr[31] == 0x98765432u && state.pc == 0x80001000u,
                        "Original memset did not preserve its return value, stack or r31.");
            }
            for (int displacement : {-8, -1, 0, 1, 8, 264}) {
                std::vector<uint8_t> expected(1024);
                for (uint32_t i = 0; i < expected.size(); ++i) {
                    expected[i] = static_cast<uint8_t>(i ^ (i >> 3));
                    ppc_write_u8(state, base + i, expected[i]);
                }
                const uint32_t source = 32u + alignment;
                const uint32_t destination = static_cast<uint32_t>(
                    static_cast<int>(source) + displacement);
                const std::vector<uint8_t> copy(expected.begin() + source,
                                                 expected.begin() + source + count);
                std::copy(copy.begin(), copy.end(), expected.begin() + destination);
                state.gpr[3] = base + destination;
                state.gpr[4] = base + source;
                state.gpr[5] = count;
                state.lr = 0x80001000u;
                ppc_800055CC(state);
                for (uint32_t i = 0; i < expected.size(); ++i) {
                    Require(ppc_read_u8(state, base + i) == expected[i],
                            "Original memcpy failed a direction, overlap or guard test.");
                }
                Require(state.gpr[3] == base + destination && state.gpr[1] == stack &&
                            state.pc == 0x80001000u,
                        "Original memcpy changed its return value or return address.");
            }
        }
    }
    std::cout << "US memcpy/memset passed zero-size, alignment, overlap, guard and return tests.\n";
}

void TestMemoryInitialization(const DolImage& image) {
    auto state = image.CreateGameCubeState();
    ppc_800032B0(state);
    BindMemoryFunctions(state);
    struct Range { uint32_t address; uint32_t size; };
    std::vector<Range> bss;
    for (uint32_t table = 0x800056a0u; table < 0x800056c0u; table += 8) {
        const uint32_t size = ppc_read_u32(state, table + 4);
        if (size == 0) {
            break;
        }
        const uint32_t address = ppc_read_u32(state, table);
        bss.push_back({address, size});
        const size_t offset = ppc_memory_offset(state, address, size);
        std::fill_n(state.memory.begin() + offset, size, 0xa5);
        ppc_write_u8(state, address - 1, 0x5a);
        ppc_write_u8(state, address + size, 0x5a);
    }
    Require(bss.size() == 3, "Verified DOL did not contain the expected three BSS ranges.");
    std::vector<std::vector<uint32_t>> text;
    for (const auto& section : image.sections()) {
        if (section.executable) {
            text.push_back(image.ReadWords(section.address, section.size));
        }
    }
    const uint32_t stack = state.gpr[1];
    state.gpr[29] = 0x29292929u;
    state.gpr[30] = 0x30303030u;
    state.gpr[31] = 0x31313131u;
    state.lr = 0x80003170u;
    ppc_80003340(state);
    for (const auto& range : bss) {
        const size_t offset = ppc_memory_offset(state, range.address, range.size);
        Require(std::all_of(state.memory.begin() + offset,
                            state.memory.begin() + offset + range.size,
                            [](uint8_t byte) { return byte == 0; }) &&
                    ppc_read_u8(state, range.address - 1) == 0x5a &&
                    ppc_read_u8(state, range.address + range.size) == 0x5a,
                "Original memory initializer did not clear precisely the BSS range.");
    }
    size_t sectionIndex = 0;
    for (const auto& section : image.sections()) {
        if (!section.executable) {
            continue;
        }
        const auto& expected = text.at(sectionIndex++);
        for (size_t i = 0; i < expected.size(); ++i) {
            Require(ppc_read_u32(state, section.address + static_cast<uint32_t>(i * 4)) ==
                        expected[i], "Memory initialization modified loaded text.");
        }
    }
    Require(state.gpr[1] == stack && state.pc == 0x80003170u &&
                state.gpr[29] == 0x29292929u && state.gpr[30] == 0x30303030u &&
                state.gpr[31] == 0x31313131u,
            "Original memory initializer damaged the ABI or continuation.");
    std::cout << "US ROM/BSS initializer cleared all three original BSS ranges; "
                 "loaded text, guards and callee-saved registers preserved.\n";

    ppc_write_u32(state, 0x8000561cu, 0x80001000u);
    ppc_write_u32(state, 0x80005620u, 0x80002000u);
    ppc_write_u32(state, 0x80005624u, 4);
    ppc_write_u32(state, 0x80001000u, 0x12345678u);
    bool refused = false;
    try {
        ppc_80003340(state);
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()) ==
                    "Unsupported startup memory dependency at " +
                        std::to_string(0x80003424u),
                "ROM-copy path failed at an unexpected dependency.");
        refused = true;
    }
    Require(refused && ppc_read_u32(state, 0x80002000u) == 0x12345678u,
            "Relocating ROM-copy path did not copy or silently skipped cache synchronization.");
    std::cout << "Relocating ROM-copy path copies data then explicitly rejects "
                 "the unimplemented cache synchronization service.\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2) {
            throw std::runtime_error("Expected verified US main.dol path.");
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) {
            throw std::runtime_error("Could not open startup test DOL.");
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        Require(Sha1Hex(bytes) == "1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374",
                "Startup tests require the verified US revision 0 DOL.");
        const auto image = DolImage::Parse(std::move(bytes));
        TestEntryAndRegisters(image);
        TestStartupHelpers(image);
        TestArgumentRelocation(image);
        TestMemoryFunctions(image);
        TestMemoryInitialization(image);
    } catch (const std::exception& error) {
        std::cerr << "US startup test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All compiled US startup tests passed. This is not a game boot.\n";
    return 0;
}
