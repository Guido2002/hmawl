#include "native_dsp_task.h"

#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

std::string RecompileDspTask(const std::vector<uint16_t>& words, uint16_t entry,
                             const std::string& functionName) {
    if (words.empty() || words.size() > 4096 || entry >= words.size() || functionName.empty() ||
        functionName.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
            std::string::npos || (functionName[0] >= '0' && functionName[0] <= '9')) {
        throw std::runtime_error("Invalid bounded native DSP task program or function name.");
    }
    std::vector<uint16_t> pending{entry};
    std::set<uint16_t> visited, operands;
    std::map<uint16_t, std::string> instructions;
    while (!pending.empty()) {
        const auto pc = pending.back();
        pending.pop_back();
        if (pc >= words.size() || operands.count(pc)) throw std::runtime_error("Invalid native DSP task branch target.");
        if (!visited.insert(pc).second) continue;
        const auto instruction = words[pc];
        unsigned length = 1;
        bool terminal = false;
        bool supported = true;
        std::ostringstream body;
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
        } else if (instruction == 0x8a00 || instruction == 0x8b00 ||
                   instruction == 0x8c00 || instruction == 0x8d00 ||
                   instruction == 0x8e00 || instruction == 0x8f00) {
            const auto mask = instruction < 0x8c00 ? 0x2000u : instruction < 0x8e00 ? 0x8000u : 0x4000u;
            body << "r[19] " << ((instruction & 0x100u) ? "|= " : "&= ~") << mask << "u;\n";
        } else if ((instruction & 0xffe0u) == 0x0080u && (instruction & 31u) != 12 &&
                   (instruction & 31u) != 13 && (instruction & 31u) != 14 && (instruction & 31u) != 15) {
            body << "NativeDspTaskWriteRegister(state, " << (instruction & 31) << ", " << immediate() << ");\n";
        } else if (instruction == 0x8100 || instruction == 0x8900) {
            const unsigned acc = (instruction >> 11) & 1;
            body << "r[" << 16 + acc << "] = r[" << 28 + acc << "] = r[" << 30 + acc << "] = 0;\n"
                 << "r[19] = (r[19] & ~0x3fu) | 0x24u;\n";
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
        } else if (instruction == 0x029f || instruction == 0x029c || instruction == 0x029d) {
            const auto target = immediate();
            pending.push_back(target);
            if (instruction == 0x029f) {
                terminal = true;
                body << "dsp.instructionPc = " << target << ";\ncontinue;\n";
            } else {
                body << "if (" << (instruction == 0x029c ? "!" : "") << "(r[19] & 0x40u)) {\n"
                     << "dsp.instructionPc = " << target << ";\n";
                if (target <= pc) body << "return;\n";
                else body << "continue;\n";
                body << "}\n";
            }
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
        if (supported) generated << "NativeDspInitStep(dsp, " << pc << ", budget);\n";
        generated << body.str();
        if (!terminal) {
            const auto next = static_cast<uint16_t>(pc + length);
            pending.push_back(next);
            generated << "dsp.instructionPc = " << next << ";\ncontinue;\n";
        }
        instructions.emplace(pc, generated.str());
    }
    std::ostringstream code;
    code << "static void " << functionName << "(PpcState& state) {\n"
            "auto& dsp = *state.dspHardware;\nauto& task = *dsp.task;\nauto& r = task.registers;\n"
            "uint32_t budget = 256;\nwhile (budget) {\nswitch (dsp.instructionPc) {\n";
    for (const auto& instruction : instructions) code << instruction.second;
    code << "default: throw std::runtime_error(\"No native DSP task translation at word \" + "
            "std::to_string(dsp.instructionPc) + \".\");\n}\n}\n}\n";
    return code.str();
}
