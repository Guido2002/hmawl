#include "native_dsp_task.h"

#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

std::string RecompileDspTask(const std::vector<uint16_t>& words, uint16_t entry,
                             const std::string& functionName, const std::vector<uint16_t>& indirectEntries) {
    if (words.empty() || words.size() > 4096 || entry >= words.size() || functionName.empty() ||
        functionName.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos || (functionName[0] >= '0' && functionName[0] <= '9')) {
        throw std::runtime_error("Invalid bounded native DSP task program or function name.");
    }
    std::vector<uint16_t> pending{entry};
    pending.insert(pending.end(), indirectEntries.begin(), indirectEntries.end());
    std::set<uint16_t> visited, operands;
    std::map<uint16_t, std::string> instructions;
    const auto instructionLength = [&](unsigned address) -> unsigned {
        if (address >= words.size()) throw std::runtime_error("Invalid native DSP loop end.");
        const auto word = words[address];
        if ((word & 0xffe0u) == 0x0080 || (word & 0xffc0u) == 0x00c0 ||
            (word & 0xff00u) == 0x1600 || (word & 0xffe0u) == 0x0060 ||
            (word & 0xff00u) == 0x1100 ||
            (word & 0xfeffu) == 0x0200 || (word & 0xfeffu) == 0x0220 ||
            (word & 0xfeffu) == 0x0260 || (word & 0xfeffu) == 0x0280 ||
            (word & 0xfeffu) == 0x0240 || (word & 0xfeffu) == 0x02a0 ||
            (word & 0xfeffu) == 0x02c0 || (word & 0xfff0u) == 0x0290 ||
            (word & 0xfff0u) == 0x02b0) return 2;
        return 1;
    };
    while (!pending.empty()) {
        const auto pc = pending.back();
        pending.pop_back();
        if (pc >= words.size() || operands.count(pc)) {
            throw std::runtime_error("Invalid native DSP task branch target at word " + std::to_string(pc) + ".");
        }
        if (!visited.insert(pc).second) continue;
        const auto instruction = words[pc];
        unsigned length = 1;
        bool terminal = false;
        bool supported = true;
        std::ostringstream body;
        std::string extensionBefore, extensionAfter;
        auto operation = instruction;
        const auto main = instruction & 0xff00u;
        const auto extension = instruction & 0xffu;
        const bool parallel = (instruction & 0xf700u) == 0x8100u ||
                              (instruction & 0xf700u) == 0xb100u ||
                              (instruction & 0xfe00u) == 0x7c00u ||
                              main == 0x8e00 || main == 0x8f00 || main == 0x8000;
        if (parallel && extension) {
            if ((extension & 0xc4u) == 0x40u) {
                const unsigned ar = extension & 3, reg = 24 + ((extension >> 3) & 7);
                extensionBefore = "if (r[" + std::to_string(8 + ar) + "] != 0xffffu) "
                    "throw std::runtime_error(\"Native DSP parallel load requires linear addressing.\");\n"
                    "const auto parallelValue = NativeDspTaskRead(state, r[" + std::to_string(ar) + "]);\n";
                extensionAfter = "NativeDspTaskWriteRegister(state, " + std::to_string(reg) +
                    ", parallelValue);\n++r[" + std::to_string(ar) + "];\n";
                operation &= 0xff00u;
            } else if ((extension & 0xe4u) == 0x20u) {
                const unsigned ar = extension & 3, reg = 28 + ((extension >> 3) & 3);
                extensionBefore = "if (r[" + std::to_string(8 + ar) + "] != 0xffffu) "
                    "throw std::runtime_error(\"Native DSP parallel store requires linear addressing.\");\n"
                    "if (r[" + std::to_string(ar) + "] >= dsp.dataRam.size()) "
                    "throw std::runtime_error(\"Native DSP parallel store requires data RAM.\");\n"
                    "const auto parallelValue = NativeDspTaskReadRegister(state, " + std::to_string(reg) + ");\n";
                extensionAfter = "NativeDspTaskWrite(state, r[" + std::to_string(ar) +
                    "], parallelValue);\n++r[" + std::to_string(ar) + "];\n";
                operation &= 0xff00u;
            }
        }
        const auto immediate = [&]() -> uint16_t {
            if (pc + 1u >= words.size() || visited.count(pc + 1)) {
                throw std::runtime_error("Truncated or overlapping native DSP task instruction.");
            }
            length = 2;
            operands.insert(pc + 1);
            return words[pc + 1];
        };
        if ((instruction & 0xfff8u) == 0x1200u || (instruction & 0xfff8u) == 0x1300u) {
            const auto mask = 1u << (6 + (instruction & 7));
            if (mask != 0x100) {
                body << "r[19] " << ((instruction & 0x100u) ? "|= " : "&= ~") << mask << "u;\n";
            }
        } else if (operation == 0x8a00 || operation == 0x8b00 ||
                   operation == 0x8c00 || operation == 0x8d00 ||
                   operation == 0x8e00 || operation == 0x8f00) {
            const auto mask = operation < 0x8c00 ? 0x2000u : operation < 0x8e00 ? 0x8000u : 0x4000u;
            body << "r[19] " << ((operation & 0x100u) ? "|= " : "&= ~") << mask << "u;\n";
        } else if ((instruction & 0xffe0u) == 0x0080u && (instruction & 31u) != 12 &&
                   (instruction & 31u) != 13 && (instruction & 31u) != 14 && (instruction & 31u) != 15) {
            body << "NativeDspTaskWriteRegister(state, " << (instruction & 31) << ", " << immediate() << ");\n";
        } else if (operation == 0x8100 || operation == 0x8900) {
            const unsigned acc = (instruction >> 11) & 1;
            body << "r[" << 16 + acc << "] = r[" << 28 + acc << "] = r[" << 30 + acc << "] = 0;\n"
                 << "r[19] = (r[19] & ~0x3fu) | 0x24u;\n";
        } else if (operation == 0x8000 || instruction == 0) {
        } else if (instruction == 0x8200 || operation == 0xb100 || operation == 0xb900 ||
                   instruction == 0xc100 || instruction == 0xd100 ||
                   instruction == 0xc900 || instruction == 0xd900) {
            if (instruction == 0x8200) {
                body << "NativeDspArithmeticFlags(state, NativeDspAccumulator(state, 0), "
                        "NativeDspAccumulator(state, 1), true);\n";
            } else if (operation == 0xb100 || operation == 0xb900) {
                body << "NativeDspArithmeticFlags(state, NativeDspAccumulator(state, "
                     << ((instruction >> 11) & 1) << "), 0, false);\n";
            } else {
                body << "NativeDspArithmeticFlags(state, NativeDspAccumulator(state, "
                     << ((instruction >> 11) & 1) << "), int64_t(static_cast<int16_t>(r["
                     << 26 + ((instruction >> 12) & 1) << "])) * 65536, true);\n";
            }
        } else if ((instruction & 0xffe0u) == 0x0040 || (instruction & 0xffe0u) == 0x0060 ||
                   (instruction & 0xff00u) == 0x1000 || (instruction & 0xff00u) == 0x1100) {
            const bool block = (instruction & 0xffe0u) == 0x0060 || (instruction & 0xff00u) == 0x1100;
            const auto end = block ? immediate() : static_cast<uint16_t>(pc + 1);
            if (end < pc + length) throw std::runtime_error("Invalid native DSP loop range.");
            const auto after = static_cast<uint16_t>(end + instructionLength(end));
            pending.push_back(static_cast<uint16_t>(pc + length));
            pending.push_back(end);
            pending.push_back(after);
            terminal = true;
            body << "NativeDspStartLoop(state, " << pc + length << ", " << end << ", ";
            if (instruction & 0x1000) body << (instruction & 0xff);
            else body << "NativeDspTaskReadRegister(state, " << (instruction & 31) << ")";
            body << ", " << after << ");\ncontinue;\n";
        } else if ((instruction & 0xff80u) == 0x1900 || (instruction & 0xff80u) == 0x1b00) {
            const auto ar = (instruction >> 5) & 3, reg = instruction & 31;
            body << "if (r[" << 8 + ar << "] != 0xffffu) "
                    "throw std::runtime_error(\"Native DSP indexed transfer requires linear addressing.\");\n";
            if (instruction & 0x0200) {
                body << "NativeDspTaskWrite(state, r[" << ar << "], NativeDspTaskReadRegister(state, " << reg << "));\n";
            } else {
                body << "const auto value = NativeDspTaskRead(state, r[" << ar << "]);\n"
                     << "NativeDspTaskWriteRegister(state, " << reg << ", value);\n";
            }
            body << "++r[" << ar << "];\n";
        } else if ((operation & 0xfeffu) == 0x7c00u) {
            const auto acc = (instruction >> 8) & 1;
            body << "const auto value = NativeDspAccumulator(state, " << acc << ");\n"
                 << "NativeDspArithmeticFlags(state, 0, value, true);\n"
                 << "NativeDspSetAccumulator(state, " << acc << ", -value);\n";
        } else if ((instruction & 0xfe00u) == 0x0400u) {
            const auto acc = (instruction >> 8) & 1;
            body << "const auto value = NativeDspAccumulator(state, " << acc << ");\n"
                 << "const int64_t addend = " << int64_t(int8_t(instruction)) * 65536 << ";\n"
                 << "NativeDspArithmeticFlags(state, value, addend, false);\n"
                 << "NativeDspSetAccumulator(state, " << acc << ", value + addend);\n";
        } else if (instruction == 0x9100 || instruction == 0x9900) {
            const auto acc = (instruction >> 11) & 1;
            body << "const auto value = NativeDspAccumulator(state, " << acc << ");\n"
                 << "const auto shifted = value >= 0 ? value / 65536 : -((-value + 65535) / 65536);\n"
                 << "NativeDspSetAccumulator(state, " << acc << ", shifted);\n"
                 << "NativeDspArithmeticFlags(state, shifted, 0, false);\n";
        } else if (instruction == 0x4c00 || instruction == 0x4d00) {
            const auto acc = (instruction >> 8) & 1;
            body << "const auto left = NativeDspAccumulator(state, " << acc << ");\n"
                 << "const auto right = NativeDspAccumulator(state, " << 1 - acc << ");\n"
                 << "NativeDspArithmeticFlags(state, left, right, false);\n"
                 << "NativeDspSetAccumulator(state, " << acc << ", left + right);\n";
        } else if ((instruction & 0xfffcu) == 0x0210u || (instruction & 0xfffcu) == 0x0310u) {
            body << "const auto address = r[" << (instruction & 3) << "];\n"
                 << "if (address >= " << words.size() << " || task.instructionRam[address] != program[address]) "
                    "throw std::runtime_error(\"Native DSP instruction-data read differs from compiled image.\");\n"
                 << "NativeDspTaskWriteRegister(state, " << 30 + ((instruction >> 8) & 1)
                 << ", program[address]);\n";
        } else if ((instruction & 0xff1fu) == 0x170fu) {
            terminal = true;
            body << "dsp.instructionPc = r[" << ((instruction >> 5) & 7) << "];\ncontinue;\n";
        } else if ((instruction & 0xffe0u) == 0x00c0u) {
            body << "NativeDspTaskWriteRegister(state, " << (instruction & 31)
                 << ", NativeDspTaskRead(state, " << immediate() << "));\n";
        } else if ((instruction & 0xfc00u) == 0x1c00u) {
            body << "NativeDspTaskWriteRegister(state, " << ((instruction >> 5) & 31)
                 << ", NativeDspTaskReadRegister(state, " << (instruction & 31) << "));\n";
        } else if ((instruction & 0xf800u) == 0x0800u) {
            body << "NativeDspTaskWriteRegister(state, " << 24 + ((instruction >> 8) & 7)
                 << ", " << uint16_t(int16_t(int8_t(instruction))) << ");\n";
        } else if ((instruction & 0xfc00u) == 0x2c00u) {
            body << "NativeDspTaskWrite(state, static_cast<uint16_t>((r[18] << 8) | "
                 << (instruction & 0xff) << "), NativeDspTaskReadRegister(state, "
                 << 28 + ((instruction >> 8) & 3) << "));\n";
        } else if (instruction == 0x0240 || instruction == 0x0340) {
            const auto reg = 30 + ((instruction >> 8) & 1);
            body << "r[" << reg << "] &= " << immediate() << "u;\n"
                 << "NativeDspLogicFlags(state, " << (reg - 30) << ");\n";
        } else if ((instruction & 0xffe0u) == 0x00e0u) {
            body << "NativeDspTaskWrite(state, " << immediate() << ", NativeDspTaskReadRegister(state, "
                 << (instruction & 31) << "));\n";
        } else if ((instruction & 0xff00u) == 0x1600u) {
            body << "NativeDspTaskWrite(state, " << (0xff00u | (instruction & 0xffu)) << ", " << immediate() << ");\n";
        } else if ((instruction & 0xf800u) == 0x2000u) {
            body << "NativeDspTaskWriteRegister(state, " << 24 + ((instruction >> 8) & 7)
                 << ", NativeDspTaskRead(state, static_cast<uint16_t>((r[18] << 8) | "
                 << (instruction & 0xff) << ")));\n";
        } else if (instruction == 0x02a0 || instruction == 0x03a0 ||
                   instruction == 0x02c0 || instruction == 0x03c0) {
            const auto value = immediate();
            body << "r[19] = (r[19] & ~0x40u) | ((r[" << 30 + ((instruction >> 8) & 1)
                 << "] & " << value << "u) == " << ((instruction & 0x40) ? value : 0) << "u ? 0x40u : 0u);\n";
        } else if ((instruction & 0xfff0u) == 0x0290u || instruction == 0x02bf) {
            const auto target = immediate();
            if (target >= 0x8000) {
                terminal = true;
                supported = false;
                body << "throw std::runtime_error(\"Native DSP task branch requires unsupported ROM code at word "
                     << target << ".\");\n";
            } else if (instruction == 0x029f || instruction == 0x02bf) {
                pending.push_back(target);
                terminal = true;
                if (instruction == 0x02bf) {
                    pending.push_back(static_cast<uint16_t>(pc + 2));
                    body << "NativeDspCall(state, " << pc + 2 << ");\n";
                }
                body << "dsp.instructionPc = " << target << ";\ncontinue;\n";
            } else {
                pending.push_back(target);
                body << "if (NativeDspCondition(state, " << (instruction & 15) << ")) {\n"
                     << "dsp.instructionPc = " << target << ";\n";
                if (target <= pc) body << "return;\n";
                else body << "continue;\n";
                body << "}\n";
            }
        } else if (instruction == 0x02df) {
            terminal = true;
            body << "dsp.instructionPc = NativeDspReturn(state);\ncontinue;\n";
        } else if (instruction == 0x0021) {
            terminal = true;
            body << "dsp.programHalted = true;\n*state.dspControl |= 4;\n"
                    "dsp.task->mode = NativeDspTaskMode::Halted;\n++dsp.completedPrograms;\nreturn;\n";
        } else {
            terminal = true;
            supported = false;
            body << "throw std::runtime_error(\"Native DSP task needs a translation at word "
                 << pc << " (instruction " << instruction << ").\");\n";
        }
        std::ostringstream generated;
        generated << "case " << pc << ":\n"
                  << "if (task.instructionRam[" << pc << "] != " << instruction << "u";
        if (length == 2) generated << " || task.instructionRam[" << pc + 1 << "] != " << words[pc + 1] << "u";
        generated << ") throw std::runtime_error(\"Native DSP task code mismatch at word " << pc << ".\");\n";
        if (supported) {
            if (terminal || (instruction & 0xfff0u) == 0x0290u) {
                generated << "if (!task.loops.empty() && task.loops.back().end == " << pc << ") "
                    "throw std::runtime_error(\"Native DSP control flow at a loop endpoint is unsupported.\");\n";
            }
            generated << "NativeDspInitStep(dsp, " << pc << ", budget);\n";
        }
        generated << "{\n" << extensionBefore << body.str() << extensionAfter;
        if (!terminal) {
            const auto next = static_cast<uint16_t>(pc + length);
            pending.push_back(next);
            generated << "dsp.instructionPc = NativeDspNext(state, " << pc << ", " << next << ");\ncontinue;\n";
        }
        generated << "}\n";
        instructions.emplace(pc, generated.str());
    }
    std::ostringstream code;
    code << "static void " << functionName << "(PpcState& state) {\n"
            "static constexpr uint16_t program[] = {";
    for (auto word : words) code << word << "u,";
    code << "};\nauto& dsp = *state.dspHardware;\nauto& task = *dsp.task;\nauto& r = task.registers;\n"
            "uint32_t budget = 256;\nwhile (budget) {\nswitch (dsp.instructionPc) {\n";
    for (const auto& instruction : instructions) code << instruction.second;
    code << "default: throw std::runtime_error(\"No native DSP task translation at word \" + "
            "std::to_string(dsp.instructionPc) + \".\");\n}\n}\n}\n";
    return code.str();
}
