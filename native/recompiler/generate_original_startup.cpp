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

void AddOriginalDependencies(const DolImage& image, const PpcState& source,
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
    // Adjacent GX attribute tables (17 each) and texcoord tables (7 and 21).
    for (uint32_t index = 0; index < 62; ++index) {
        const auto target = ppc_read_u32(source, 0x802e1be8u + index * 4);
        leaders.insert(target);
        pending.push_back(target);
    }
    // Original GXInitTexObj format-to-tile switch, bounded by its cmplwi guard.
    for (uint32_t index = 0; index < 15; ++index) {
        const auto target = ppc_read_u32(source, 0x802e20ccu + index * 4);
        leaders.insert(target);
        pending.push_back(target);
    }
    // The original VI timing selector guards this 27-entry indirect branch table.
    for (uint32_t index = 0; index < 27; ++index) {
        const auto target = ppc_read_u32(source, 0x802e127cu + index * 4);
        leaders.insert(target);
        pending.push_back(target);
    }
    // Original collision-reset switch, guarded to ten cases by cmplwi.
    for (uint32_t index = 0; index < 10; ++index) {
        const auto target = ppc_read_u32(source, 0x80299c40u + index * 4);
        if ((target & 3u) != 0 || target < 0x800207c4u || target >= 0x800207f4u) {
            throw std::runtime_error("Original collision-reset switch exceeds its bounded case range.");
        }
        leaders.insert(target);
        pending.push_back(target);
    }
    // DVD read eligibility switch, guarded by the original command-status range check.
    for (unsigned index = 0; index < 13; ++index) {
        const auto target = ppc_read_u32(source, 0x802c9da4u + index * 4);
        if (target != 0x8017b0f4u && target != 0x8017b0fcu) {
            throw std::runtime_error("Original DVD read-status table exceeds its bounded case range.");
        }
        leaders.insert(target);
        pending.push_back(target);
    }
    // Original DVD cancellation/completed-read switch, also guarded to 13 statuses.
    for (unsigned index = 0; index < 13; ++index) {
        const auto target = ppc_read_u32(source, 0x802e0f68u + index * 4);
        if ((target & 3u) != 0 || target < 0x801bd21cu || target > 0x801bd420u) {
            throw std::runtime_error("Original DVD cancellation table exceeds its bounded case range.");
        }
        leaders.insert(target);
        pending.push_back(target);
    }
    // Post-system-init object constructors, without pulling in the entire main-loop graph.
    for (uint32_t target : {0x80086044u, 0x8017cd5cu, 0x801770f8u, 0x801548a4u,
                            0x80140b20u, 0x8012f3e0u, 0x8012ac64u, 0x80197150u, 0x80174d64u,
                            0x80175530u, 0x80175548u, 0x800154a0u, 0x80148f78u,
                            0x80149178u, 0x8014919cu, 0x8017617cu, 0x8017767cu,
                            0x80176754u, 0x801766c0u, 0x8000cca4u, 0x80012568u,
                            0x8017bd04u, 0x8017d3e8u, 0x8019b560u,
                            0x800126ecu, 0x800126fcu, 0x8017a398u, 0x801b4860u}) {
        leaders.insert(target);
        pending.push_back(target);
    }
    // Indirect constructors, reset vtables and interrupt callbacks are original code.
    for (uint32_t target : {0x8000ad40u, 0x80199eecu, 0x8001b920u, 0x80022790u, 0x800226f0u,
                            0x8002b80cu, 0x8002f27cu, 0x8002f100u, 0x8002efbcu,
                            0x80179ee8u, 0x80179c90u, 0x801059a4u,
                            0x8019ad5cu, 0x80190204u, 0x8000b4c4u, 0x80198f54u,
                            0x80199ed8u, 0x8017a3a0u,
                            0x801cc448u, 0x801ce08cu, 0x801ce158u,
                            0x801cb0c8u, 0x801cb1c4u, 0x801c1b44u, 0x801c3f64u,
                            0x801f2b60u, 0x801f2b70u,
                            0x801c1bc0u, 0x801f2ab8u, 0x801f0df4u,
                            0x801f3438u,
                            0x801c20ecu, 0x801c3a5cu,
                            0x801f1decu,
                            0x8018101cu,
                            0x801be410u, 0x801bde64u, 0x801c0044u, 0x801b9a68u,
                            0x801baa9cu, 0x801bd4f4u, 0x8017acf0u, 0x8017b30cu, 0x8017b558u,
                            0x8000c9c4u, 0x8000c920u, 0x8000c82cu, 0x8000c788u,
                            0x80021ab0u, 0x80021ef8u, 0x80021f48u, 0x80022004u,
                            0x80022158u, 0x800222acu, 0x801809b0u, 0x801809c8u,
                            0x8020b984u, 0x8020b72cu, 0x802191a0u, 0x802197bcu,
                            0x8021b4e4u, 0x8021b3e8u, 0x8021b440u, 0x8021b448u,
                            0x8021b44cu, 0x8021b4b8u}) {
        leaders.insert(target);
        pending.push_back(target);
    }
    std::set<uint32_t> visited;
    for (size_t index = 0; index < pending.size(); ++index) {
        const auto address = pending[index];
        if (!visited.insert(address).second) continue;
        if (address == 0x802383c0u) continue; // Existing original-word-verified native console boundary.
        if (visited.size() > 100000) throw std::runtime_error("Original startup compilation exceeds its code bound.");
        const auto word = image.ReadWords(address, 4).front();
        try {
            RecompileFunction(address, {word}, {true, true});
        } catch (const UnsupportedPpcInstruction& error) {
            std::cout << "Original dependency remains untranslated: " << error.what() << '\n';
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
            if (opcode == 19 && extended == 50) continue;
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
        AddOriginalDependencies(image, jumpTableState, instructions, leaders);
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
        std::vector<uint16_t> taskProgram;
        const auto taskBytes = ppc_read_u16(dspSource, 0x80348cd0u);
        for (uint32_t offset = 0; offset < taskBytes; offset += 2) {
            taskProgram.push_back(ppc_read_u16(dspSource, 0x802e3da0u + offset));
        }
        std::vector<uint16_t> taskCommands;
        for (unsigned i = 0; i < 19; ++i) taskCommands.push_back(taskProgram.at(0xaff + i));
        for (unsigned i = 0; i < 4; ++i) taskCommands.push_back(taskProgram.at(0xc8d + i));
        functions << RecompileDspTask(taskProgram, 0x10, "OriginalDspAudioTask", taskCommands);
        functions << "static void ValidateOriginalDspAudioTask(const PpcState& state, "
                     "uint32_t source, uint16_t destination, uint16_t bytes, uint16_t entry) {\n"
                     "static constexpr uint16_t expected[] = {";
        for (const auto word : taskProgram) functions << word << "u,";
        functions << "};\nif (destination != 0 || bytes != sizeof(expected) || entry != 0x10)\n"
                     "throw std::runtime_error(\"Unsupported native DSP task image bounds or entry.\");\n"
                     "for (uint32_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i)\n"
                     "if (ppc_read_u16(state, source + 2 * i) != expected[i])\n"
                     "throw std::runtime_error(\"Native DSP task upload differs from build-time compiled DOL microcode.\");\n}\n";
        functions << "static void ExecuteOriginalDspInitialization(PpcState& state) {\n"
                     "    static constexpr uint16_t expected[] = {";
        for (const auto word : dspProgram) functions << word << "u,";
        functions << "};\n"
                     "    if (state.dspControl && (*state.dspControl & 0x800u)) {\n"
                     "        NativeDspStartTaskLoader(state);\n        return;\n    }\n"
                     "    if (!state.dspHardware || !state.dspControl || "
                     "(*state.dspControl & 0x805u) != 0 || "
                     "state.dspHardware->aramConfiguration != 0x43u)\n"
                     "        throw std::runtime_error(\"Unsupported native DSP bootstrap handoff.\");\n"
                     "    for (size_t i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {\n"
                     "        if (ppc_read_u16(state, 0x81000000u + static_cast<uint32_t>(2 * i)) != expected[i])\n"
                     "            throw std::runtime_error(\"Native DSP bootstrap payload differs from compiled DOL microcode.\");\n"
                     "    }\n"
                     "    OriginalDspInitialization(*state.dspHardware);\n}\n";
        bindings << "#if defined(_MSC_VER)\n#define HMAWL_BIND_NOINLINE __declspec(noinline)\n"
                    "#else\n#define HMAWL_BIND_NOINLINE __attribute__((noinline))\n#endif\n";
        size_t bindingCount = 0, batchCount = 0;
        for (auto at = instructions.begin(); at != instructions.end();) {
            if (bindingCount % 64 == 0) {
                if (bindingCount != 0) bindings << "}\n";
                bindings << "static HMAWL_BIND_NOINLINE void OriginalBindings" << batchCount++
                         << "(std::vector<NativeTranslation>& translations) {\n";
            }
            ++bindingCount;
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
            bindings << "    translations.push_back({0x" << Hex(address) << "u, ppc_" << Hex(address) << ", {";
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
            bindings << "});\n";
        }
        bindings << "    translations.push_back({0x802383C0u, NativeGuestVprintf, {";
        for (auto word : image.ReadWords(0x802383c0u, 0x7cu)) {
            bindings << "0x" << Hex(word) << "u,";
        }
        bindings << "}});\n}\n#undef HMAWL_BIND_NOINLINE\n"
                    "static std::vector<NativeTranslation> OriginalStartupTranslations() {\n"
                    "    std::vector<NativeTranslation> translations;\n"
                 << "    translations.reserve(" << bindingCount + 1 << ");\n";
        for (size_t batch = 0; batch < batchCount; ++batch) {
            bindings << "    OriginalBindings" << batch << "(translations);\n";
        }
        bindings << "    return translations;\n}\n";
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
