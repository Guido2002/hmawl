#include "dol_image.h"
#include "dsp_init_recompiler.h"
#include "original_startup_manifest.h"
#include "ppc_recompiler.h"
#include "sha1.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace {

std::string Hex(uint32_t value) {
    std::ostringstream text;
    text << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

bool IsBranch(uint32_t word) {
    const auto opcode = word >> 26;
    const auto extended = (word >> 1) & 1023u;
    return opcode == 16 || opcode == 17 || opcode == 18 ||
           (opcode == 19 && (extended == 16 || extended == 50 || extended == 528));
}

uint32_t BranchTarget(uint32_t address, uint32_t word) {
    const uint32_t mask = (word >> 26) == 18 ? 0x03fffffcu : 0xfffcu;
    const uint32_t sign = (mask + 4u) >> 1;
    const uint32_t displacement = ((word & mask) ^ sign) - sign;
    return (word & 2u) ? displacement : address + displacement;
}

void AddOriginalConstructors(const DolImage& image, const PpcState& source,
                             std::map<uint32_t, uint32_t>& instructions,
                             std::set<uint32_t>& leaders) {
    std::vector<uint32_t> pending;
    bool terminated = false;
    for (uint32_t offset = 0; offset < 0x180u; offset += 4) {
        const auto target = ppc_read_u32(source, 0x8023dd80u + offset);
        if (target == 0) {
            terminated = true;
        } else {
            if (terminated) throw std::runtime_error("Original constructor table has data after its terminator.");
            leaders.insert(target);
            pending.push_back(target);
        }
    }
    if (!terminated || pending.size() != 93) {
        throw std::runtime_error("Original USA constructor table differs from its bounded native profile.");
    }
    // Original virtual and array-construction calls do not encode their targets in a branch.
    for (uint32_t target : {0x80199eecu, 0x8001b920u, 0x80022790u, 0x800226f0u,
                            0x8002b80cu, 0x8002f27cu, 0x8002f100u, 0x8002efbcu,
                            0x80179ee8u, 0x80179c90u, 0x801059a4u}) {
        leaders.insert(target);
        pending.push_back(target);
    }
    std::set<uint32_t> visited;
    for (size_t index = 0; index < pending.size(); ++index) {
        const auto address = pending[index];
        if (!visited.insert(address).second) continue;
        if (visited.size() > 50000) throw std::runtime_error("Original constructor compilation exceeds its code bound.");
        const auto word = image.ReadWords(address, 4).front();
        try {
            RecompileFunction(address, {word}, {true, true});
        } catch (const UnsupportedPpcInstruction& error) {
            std::cout << "Constructor dependency remains untranslated: " << error.what() << '\n';
            continue;
        }
        instructions.emplace(address, word);
        const auto opcode = word >> 26;
        const auto extended = (word >> 1) & 1023u;
        if (opcode == 16 || opcode == 18) {
            pending.push_back(BranchTarget(address, word));
            if (opcode == 16 || (word & 1u) != 0) pending.push_back(address + 4);
        } else if (opcode == 19 && (extended == 16 || extended == 528)) {
            if ((word & 1u) != 0 || ((word >> 21) & 20u) != 20u) pending.push_back(address + 4);
        } else {
            if (opcode == 17 || (opcode == 19 && extended == 50)) {
                throw std::runtime_error("Unexpected exception transfer in original constructor dependencies.");
            }
            pending.push_back(address + 4);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Expected <US-main.dol> <generated-output>.");
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Could not open original startup DOL.");
        const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        if (Sha1Hex(bytes) != kOriginalUsDolSha1) {
            throw std::runtime_error("Original startup requires the verified USA revision 0 DOL.");
        }
        const auto image = DolImage::Parse(bytes);
        std::map<uint32_t, uint32_t> instructions;
        std::set<uint32_t> leaders;
        for (const auto& range : kOriginalStartupRanges) {
            leaders.insert(range.address);
            const auto words = image.ReadWords(range.address, range.size);
            for (size_t i = 0; i < words.size(); ++i) {
                instructions.emplace(range.address + static_cast<uint32_t>(i * 4), words[i]);
            }
        }
        const auto systemCallWords = image.ReadWords(0x801b6968u, 0x1cu);
        for (size_t i = 0; i < systemCallWords.size(); ++i) {
            instructions.emplace(0xc00u + static_cast<uint32_t>(i * 4), systemCallWords[i]);
        }
        leaders.insert(0xc00u);
        const auto interruptVector = image.ReadWords(0x801b1490u, 0x98u);
        for (size_t i = 0; i < interruptVector.size(); ++i) {
            auto word = interruptVector[i];
            if (i == (0x801b14f8u - 0x801b1490u) / 4u) word |= 4u;
            instructions.emplace(0x500u + static_cast<uint32_t>(i * 4), word);
        }
        leaders.insert(0x500u);
        const auto ramConfiguration = image.ReadWords(0x801b4f54u, 0x80u);
        for (size_t i = 0; i < ramConfiguration.size(); ++i) {
            instructions.emplace(0x001b4f54u + static_cast<uint32_t>(i * 4), ramConfiguration[i]);
        }
        leaders.insert(0x001b4f54u);
        const auto jumpTableState = image.CreateGameCubeState();
        AddOriginalConstructors(image, jumpTableState, instructions, leaders);
        leaders.insert(0x80003268u);
        leaders.insert(0x800032a0u);
        for (uint32_t offset = 0; offset < 16u * 4u; offset += 4) {
            const auto target = ppc_read_u32(jumpTableState, 0x802e0ed8u + offset);
            if (target < 0x801bc080u || target >= 0x801bc3a0u || (target & 3u) != 0) {
                throw std::runtime_error("Original DVD command table target is outside its compiled function.");
            }
            leaders.insert(target);
        }
        for (const auto& instruction : instructions) {
            if (IsBranch(instruction.second)) {
                leaders.insert(instruction.first + 4);
                const auto opcode = instruction.second >> 26;
                if (opcode == 16 || opcode == 18) {
                    leaders.insert(BranchTarget(instruction.first, instruction.second));
                }
            }
        }
        std::ostringstream functions, bindings;
        std::vector<uint16_t> dspProgram;
        const auto dspSource = image.CreateGameCubeState();
        for (uint32_t offset = 0; offset < 0x80u; offset += 2) {
            dspProgram.push_back(ppc_read_u16(dspSource, 0x802e0228u + offset));
        }
        functions << RecompileDspInitialization(dspProgram, "OriginalDspInitialization");
        functions << "static void ExecuteOriginalDspInitialization(PpcState& state) {\n"
                     "    static constexpr uint16_t expected[] = {";
        for (const auto word : dspProgram) functions << word << "u,";
        functions << "};\n"
                     "    if (!state.dspHardware || !state.dspControl || "
                     "(*state.dspControl & 0x805u) != 0 || "
                     "state.dspHardware->aramConfiguration != 0x43u)\n"
                     "        throw std::runtime_error(\"Unsupported native DSP bootstrap handoff.\");\n"
                     "    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {\n"
                     "        if (ppc_read_u16(state, 0x81000000u + static_cast<uint32_t>(2 * i)) != expected[i])\n"
                     "            throw std::runtime_error(\"Native DSP bootstrap payload differs from compiled DOL microcode.\");\n"
                     "    }\n"
                     "    OriginalDspInitialization(*state.dspHardware);\n}\n";
        bindings << "static std::vector<NativeTranslation> OriginalStartupTranslations() {\n"
                    "    return {\n";
        for (auto at = instructions.begin(); at != instructions.end();) {
            const uint32_t address = at->first;
            std::vector<uint32_t> words;
            do {
                const uint32_t previousAddress = at->first;
                const uint32_t word = at->second;
                words.push_back(word);
                ++at;
                if (IsBranch(word) || words.size() == 32 || at == instructions.end() ||
                    at->first != previousAddress + 4 || leaders.count(at->first)) break;
            } while (true);
            functions << RecompileFunction(address, words, {true, true}) << '\n';
            bindings << "        {0x" << Hex(address) << "u, ppc_" << Hex(address) << ", {";
            for (auto word : words) bindings << "0x" << Hex(word) << "u,";
            bindings << "}";
            if (address >= 0xc00u && address < 0xc1cu) {
                bindings << ",0x" << Hex(0x801b6968u + address - 0xc00u) << "u";
            } else if (address >= 0x500u && address < 0x598u) {
                const auto source = 0x801b1490u + address - 0x500u;
                bindings << ",0x" << Hex(source) << "u,{";
                for (auto word : image.ReadWords(source, static_cast<uint32_t>(words.size() * 4u))) {
                    bindings << "0x" << Hex(word) << "u,";
                }
                bindings << "}";
            }
            bindings << "},\n";
        }
        bindings << "        {0x802383C0u, NativeGuestVprintf, {";
        for (auto word : image.ReadWords(0x802383c0u, 0x7cu)) {
            bindings << "0x" << Hex(word) << "u,";
        }
        bindings << "}},\n    };\n}\n";
        std::ofstream output(argv[2], std::ios::binary);
        output << "#include \"native_dispatch.h\"\n#include \"native_console.h\"\n"
                  "#include \"native_dsp_init.h\"\n\n"
               << functions.str() << bindings.str();
        output.close();
        if (!output) throw std::runtime_error("Could not write original startup bindings.");
    } catch (const std::exception& error) {
        std::cerr << "Original startup generation error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
