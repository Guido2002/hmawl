#include "ppc_recompiler.h"
#include "rel_image.h"
#include "sha1.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <set>
#include <sstream>

namespace {

constexpr uint32_t kBase = 0x81000000u;

std::string Hex(uint32_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << std::uppercase << value << "u";
    return text.str();
}

uint32_t Read32(const std::vector<uint8_t>& bytes, uint32_t offset) {
    return (uint32_t(bytes.at(offset)) << 24) | (uint32_t(bytes.at(offset + 1)) << 16) |
        (uint32_t(bytes.at(offset + 2)) << 8) | bytes.at(offset + 3);
}

std::string Section(unsigned section) {
    return "(ppc_read_u32(state, relocationBase + 0x4Cu + " +
        std::to_string(section * 8) + "u) & ~1u)";
}

std::string Target(const RelRelocation& fixup) {
    if (fixup.importId == 0) return Hex(fixup.addend);
    if (fixup.importId != 2) throw std::runtime_error("Scene translation needs an unloaded module import.");
    return "(" + Section(fixup.targetSection) + " + " + Hex(fixup.addend) + ")";
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Expected verified wlp1.rel and generated header path.");
        if (std::filesystem::file_size(argv[1]) != 433296u) {
            throw std::runtime_error("Original scene REL size differs from the USA image.");
        }
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) throw std::runtime_error("Could not open original scene REL.");
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
        if (input.bad() || Sha1Hex(bytes) != "509f77d903c89472be5590d45ddf65ddb2e93055") {
            throw std::runtime_error("Original scene requires the verified USA wlp1.rel.");
        }
        const auto image = RelImage::Parse(bytes);
        const auto& code = image.sections().at(1);
        const auto start = kBase + code.fileOffset;
        const auto end = start + code.size;
        std::map<uint32_t, RelRelocation> fixups;
        for (const auto& fixup : image.relocations()) {
            if (fixup.patchSection == 1) {
                if ((fixup.type == 10 && (fixup.patchOffset & 3u) != 0) ||
                    ((fixup.type == 4 || fixup.type == 6) && (fixup.patchOffset & 3u) != 2)) {
                    throw std::runtime_error("Misaligned original scene instruction relocation.");
                }
                const auto pc = start + (fixup.patchOffset & ~3u);
                if (!fixups.emplace(pc, fixup).second) {
                    throw std::runtime_error("Overlapping original scene instruction relocations.");
                }
            }
        }
        const auto canonicalTarget = [&](const RelRelocation& fixup) {
            if (fixup.importId == 0) return fixup.addend;
            if (fixup.importId != 2) throw std::runtime_error("Unloaded module in original scene code.");
            const auto& section = image.sections().at(fixup.targetSection);
            return kBase + (section.fileOffset ? section.fileOffset : 0x100000u) + fixup.addend;
        };
        const auto wordAt = [&](uint32_t pc) {
            uint32_t word = Read32(bytes, pc - kBase);
            const auto found = fixups.find(pc);
            if (found != fixups.end()) {
                const auto& fixup = found->second;
                const auto target = canonicalTarget(fixup);
                if (fixup.type == 10) word = (word & 0xfc000003u) | ((target - pc) & 0x03fffffcu);
                else if (fixup.type == 4) word = (word & 0xffff0000u) | (target & 0xffffu);
                else if (fixup.type == 6) word = (word & 0xffff0000u) |
                    (((target >> 16) + ((target & 0x8000u) ? 1u : 0u)) & 0xffffu);
                else throw std::runtime_error("Unsupported original scene instruction relocation.");
            }
            return word;
        };
        RecompileOptions options{true, true};
        options.relocationBase = kBase;
        options.addressExpression = [&](uint32_t address) {
            return address >= start && address <= end ?
                "(relocationBase + " + Hex(address - kBase) + ")" : Hex(address);
        };
        options.immediate16Expression = [&](uint32_t pc) -> std::string {
            const auto found = fixups.find(pc);
            if (found == fixups.end() || found->second.type == 10) return {};
            // The dispatcher guards these exact operand bytes before executing the fixed native operation.
            return "uint32_t{ppc_read_u16(state, relocationBase + " + Hex(pc - kBase + 2) + ")}";
        };
        // Original mode-41 dispatcher case and its state-24 asset setup case.
        std::vector<uint32_t> pending = {start + image.entryOffsets()[0],
            kBase + 0x45b8cu, kBase + 0x46040u, kBase + 0x28d0u,
            kBase + 0x954u, kBase + 0x958u, kBase + 0x263cu, kBase + 0x3e734u,
            kBase + 0x42ecu, kBase + 0x42f4u};
        const auto& ctors = image.sections().at(2);
        for (uint32_t offset = 0; offset < ctors.size; offset += 4) {
            const auto found = std::find_if(image.relocations().begin(), image.relocations().end(),
                [&](const RelRelocation& fixup) {
                    return fixup.patchSection == 2 && fixup.patchOffset == offset;
                });
            if (found != image.relocations().end()) pending.push_back(canonicalTarget(*found));
        }
        // Original asset-update state switch, guarded to seven cases by cmplwi.
        const auto& data = image.sections().at(5);
        for (uint32_t index = 0; index < 7; ++index) {
            const auto found = std::find_if(image.relocations().begin(), image.relocations().end(),
                [&](const RelRelocation& fixup) {
                    return fixup.patchSection == 5 &&
                        fixup.patchOffset == 0x4bbe8u - data.fileOffset + index * 4;
                });
            if (found == image.relocations().end()) {
                throw std::runtime_error("Missing original asset-update switch relocation.");
            }
            const auto target = canonicalTarget(*found);
            if (found->type != 1 || target < kBase + 0x3e778u ||
                target > kBase + 0x3e7c4u || (target & 3u) != 0) {
                throw std::runtime_error("Original asset-update switch exceeds its bounded case range.");
            }
            pending.push_back(target);
        }
        std::set<uint32_t> visited, leaders(pending.begin(), pending.end());
        std::map<uint32_t, uint32_t> instructions;
        while (!pending.empty()) {
            const auto pc = pending.back();
            pending.pop_back();
            if (pc < start || pc >= end || !visited.insert(pc).second) continue;
            if (visited.size() > 100000) throw std::runtime_error("Scene discovery exceeds its instruction bound.");
            const auto word = wordAt(pc);
            try {
                RecompileFunction(pc, {word}, options);
            } catch (const UnsupportedPpcInstruction& error) {
                std::cerr << "Original scene dependency remains untranslated: " << error.what() << '\n';
                continue;
            }
            instructions.emplace(pc, word);
            const auto opcode = word >> 26;
            const auto extended = (word >> 1) & 1023u;
            if (opcode == 16 || opcode == 18) {
                const uint32_t mask = opcode == 18 ? 0x03fffffcu : 0xfffcu;
                const auto sign = (mask + 4u) >> 1;
                const auto displacement = ((word & mask) ^ sign) - sign;
                const auto target = (word & 2u) ? displacement : pc + displacement;
                leaders.insert(target);
                pending.push_back(target);
                if (opcode == 16 || (word & 1u)) {
                    leaders.insert(pc + 4);
                    pending.push_back(pc + 4);
                }
            } else if (opcode == 19 && (extended == 16 || extended == 528)) {
                if (word & 1u) {
                    leaders.insert(pc + 4);
                    pending.push_back(pc + 4);
                }
            } else {
                pending.push_back(pc + 4);
            }
        }
        std::ostringstream functions, bindings;
        bindings << "static std::vector<NativeTranslation> OriginalSceneTranslations("
                    "const PpcState& state, uint32_t relocationBase) {\n"
                    "    if (ppc_read_u32(state, relocationBase) != 2u || "
                    "ppc_read_u32(state, relocationBase + 0x10u) != relocationBase + 0x4Cu) "
                    "throw std::runtime_error(\"Original scene module/header mismatch.\");\n"
                    "    return {\n";
        for (auto it = instructions.begin(); it != instructions.end();) {
            const auto address = it->first;
            std::vector<uint32_t> words;
            do {
                words.push_back(it->second);
                const auto previous = it++->first;
                if (it == instructions.end() || it->first != previous + 4 || leaders.count(it->first)) break;
            } while (true);
            functions << RecompileFunction(address, words, options) << '\n';
            bindings << "        {relocationBase + " << Hex(address - kBase)
                     << ", ppc_" << std::hex << std::uppercase << address << ", {";
            for (size_t i = 0; i < words.size(); ++i) {
                if (i) bindings << ", ";
                const auto pc = address + static_cast<uint32_t>(i * 4);
                const auto found = fixups.find(pc);
                if (found == fixups.end()) bindings << Hex(words[i]);
                else if (found->second.type == 10) {
                    bindings << "(" << Hex(words[i] & 0xfc000003u) << " | (("
                             << Target(found->second) << " - (relocationBase + " << Hex(pc - kBase)
                             << ")) & 0x03fffffcu))";
                } else {
                    const auto target = Target(found->second);
                    bindings << "(" << Hex(words[i] & 0xffff0000u) << " | "
                             << (found->second.type == 4 ? "(" + target + " & 0xffffu)" :
                                 "(((" + target + " >> 16) + ((" + target +
                                 " & 0x8000u) ? 1u : 0u)) & 0xffffu)") << ")";
                }
            }
            bindings << "}},\n";
        }
        bindings << "    };\n}\n";
        std::ofstream output(argv[2], std::ios::trunc);
        output << "#include \"native_dispatch.h\"\n" << functions.str() << bindings.str();
        if (!output) throw std::runtime_error("Could not write generated original scene translations.");
        std::cout << "Compiled " << instructions.size() << " original relocatable scene instructions.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
