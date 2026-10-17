#include "dol_image.h"
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
    return opcode == 16 || opcode == 18 ||
           (opcode == 19 && (extended == 16 || extended == 528));
}

uint32_t BranchTarget(uint32_t address, uint32_t word) {
    const uint32_t mask = (word >> 26) == 18 ? 0x03fffffcu : 0xfffcu;
    const uint32_t sign = (mask + 4u) >> 1;
    const uint32_t displacement = ((word & mask) ^ sign) - sign;
    return (word & 2u) ? displacement : address + displacement;
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
            bindings << "}},\n";
        }
        bindings << "        {0x802383C0u, NativeGuestVprintf, {";
        for (auto word : image.ReadWords(0x802383c0u, 0x7cu)) {
            bindings << "0x" << Hex(word) << "u,";
        }
        bindings << "}},\n    };\n}\n";
        std::ofstream output(argv[2], std::ios::binary);
        output << "#include \"native_dispatch.h\"\n#include \"native_console.h\"\n\n"
               << functions.str() << bindings.str();
        output.close();
        if (!output) throw std::runtime_error("Could not write original startup bindings.");
    } catch (const std::exception& error) {
        std::cerr << "Original startup generation error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
