#include "dsp_init_recompiler.h"

#include <iomanip>
#include <array>
#include <sstream>
#include <stdexcept>

namespace {

std::string Hex(uint16_t value) {
    std::ostringstream result;
    result << "0x" << std::hex << std::uppercase << value << "u";
    return result.str();
}

std::string Register(unsigned reg) {
    return "r[" + std::to_string(reg) + "]";
}

} // namespace

std::string RecompileDspInitialization(const std::vector<uint16_t>& words,
                                      const std::string& functionName) {
    if (words.size() > 0x1000 || words.size() < 3 || words[0] != 0x029fu ||
        words[1] < 2 || words[1] >= words.size() || functionName.empty() ||
        functionName.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != std::string::npos ||
        (functionName[0] >= '0' && functionName[0] <= '9')) {
        throw std::runtime_error("Invalid bounded DSP initialization program or function name.");
    }
    const auto word = [&](size_t pc) {
        if (pc >= words.size()) throw std::runtime_error("Truncated DSP initialization instruction.");
        return words[pc];
    };
    const auto reject = [](size_t pc) {
        throw std::runtime_error("Unsupported DSP initialization instruction or data flow at word " +
                                 std::to_string(pc) + ".");
    };
    std::ostringstream code;
    code << "static void " << functionName << "(PpcDspHardware& dsp) {\n"
            "    std::array<uint16_t, 32> r = {};\n"
            "    uint32_t budget = 65536;\n"
            "    NativeDspInitStep(dsp, 0, budget);\n";
    bool logicalFlagDefined = false;
    std::array<bool, 32> initialized = {};
    size_t mailboxRead = words.size();
    unsigned mailboxRegister = 0;
    for (size_t pc = words[1]; pc < words.size();) {
        const uint16_t instruction = word(pc);
        code << "dsp_" << pc << ":\n    NativeDspInitStep(dsp, " << pc << ", budget);\n";
        if ((instruction & 0xff00u) == 0x1200u && (instruction & 0xffu) <= 7) {
            code << "    r[19] &= ~" << Hex(static_cast<uint16_t>(1u << (6 + (instruction & 7))))
                 << ";\n";
            ++pc;
        } else if ((instruction & 0xffe0u) == 0x0080u) {
            if ((instruction & 31) > 11) reject(pc);
            code << "    " << Register(instruction & 31) << " = " << Hex(word(pc + 1)) << ";\n";
            initialized[instruction & 31] = true;
            pc += 2;
        } else if ((instruction & 0xffe0u) == 0x0060u) {
            const unsigned count = instruction & 31;
            const uint16_t load = word(pc + 2);
            const unsigned ar = load & 3;
            const unsigned accumulator = (load >> 8) & 1;
            const bool instructionRom = (load & 0xfefcu) == 0x0218u;
            const bool dataRom = (load & 0xff80u) == 0x1900u && (load & 31) == 30;
            const uint16_t overwrite = word(pc + 4);
            const bool cleared = instructionRom &&
                (overwrite & 0xf7ffu) == 0x8100u && ((overwrite >> 11) & 1) == accumulator;
            const bool replaced = dataRom && overwrite == 0x00deu && word(pc + 5) == 0xfffcu;
            if (word(pc + 1) != pc + 3 || word(pc + 3) != 0 ||
                (!cleared && !replaced)) reject(pc);
            const unsigned addressRegister = dataRom ? ((load >> 5) & 3) : ar;
            if (!initialized[count] || !initialized[addressRegister] ||
                !initialized[8 + addressRegister]) reject(pc);
            const uint16_t begin = instructionRom ? 0x8000u : 0x1000u;
            const uint16_t end = instructionRom ? 0x9000u : 0x1800u;
            // The only accumulator writes are discarded by the immediately following
            // CLR/LR. No flag, memory or control-flow dependency observes the ROM data.
            code << "    if (" << Register(8 + addressRegister) << " != 0xffffu || "
                 << Register(addressRegister) << " < " << Hex(begin) << " || "
                 << "static_cast<uint32_t>(" << Register(addressRegister) << ") + "
                 << Register(count) << " > " << Hex(end) << ")\n"
                    "        throw std::runtime_error(\"Eliminated DSP ROM reads exceeded their proven region.\");\n"
                    "    for (uint32_t remaining = " << Register(count) << "; remaining; --remaining) {\n"
                 << "        NativeDspInitStep(dsp, " << pc + 2 << ", budget);\n"
                 << "        ++" << Register(addressRegister) << ";\n"
                 << "        NativeDspInitStep(dsp, " << pc + 3 << ", budget);\n"
                    "    }\n";
            pc += 4;
        } else if ((instruction & 0xf7ffu) == 0x8100u) {
            const unsigned accumulator = (instruction >> 11) & 1;
            code << "    r[" << 16 + accumulator << "] = r[" << 28 + accumulator
                 << "] = r[" << 30 + accumulator << "] = 0;\n";
            initialized[16 + accumulator] = initialized[28 + accumulator] =
                initialized[30 + accumulator] = true;
            ++pc;
        } else if ((instruction & 0xfc00u) == 0x1c00u) {
            const unsigned destination = (instruction >> 5) & 31;
            if (destination > 3 || (instruction & 31) < 30 ||
                !initialized[instruction & 31]) reject(pc);
            code << "    " << Register(destination) << " = " << Register(instruction & 31) << ";\n";
            initialized[destination] = true;
            ++pc;
        } else if ((instruction & 0xffe0u) == 0x0040u) {
            const unsigned count = instruction & 31;
            const uint16_t store = word(pc + 1);
            if ((store & 0xff80u) != 0x1b00u || (store & 31) < 30) reject(pc);
            const unsigned ar = (store >> 5) & 3;
            if (!initialized[count] || !initialized[ar] || !initialized[8 + ar] ||
                !initialized[store & 31]) reject(pc);
            code << "    if (" << Register(8 + ar) << " != 0xffffu || "
                 << "static_cast<uint32_t>(" << Register(ar) << ") + " << Register(count)
                 << " > dsp.dataRam.size())\n"
                    "        throw std::runtime_error(\"Native DSP initialization store range is unsupported.\");\n"
                    "    for (uint32_t remaining = " << Register(count) << "; remaining; --remaining) {\n"
                 << "        NativeDspInitStep(dsp, " << pc + 1 << ", budget);\n"
                 << "        dsp.dataRam[" << Register(ar) << "++] = " << Register(store & 31) << ";\n"
                    "    }\n";
            pc += 2;
        } else if (instruction == 0x00deu && word(pc + 1) == 0xfffcu) {
            code << "    r[30] = NativeDspReadOutputHigh(dsp);\n";
            mailboxRead = pc;
            mailboxRegister = 30;
            initialized[30] = true;
            pc += 2;
        } else if (instruction == 0x02a0u) {
            if (!initialized[30]) reject(pc);
            code << "    r[19] = static_cast<uint16_t>((r[19] & ~0x40u) | "
                    "((r[30] & " << Hex(word(pc + 1)) << ") == 0 ? 0x40u : 0u));\n";
            logicalFlagDefined = true;
            pc += 2;
        } else if (instruction == 0x029cu) {
            if (!logicalFlagDefined || word(pc + 1) != mailboxRead ||
                mailboxRegister != 30 || pc != mailboxRead + 4 ||
                word(mailboxRead + 2) != 0x02a0u) reject(pc);
            code << "    if ((r[19] & 0x40u) == 0) goto dsp_" << mailboxRead << ";\n";
            pc += 2;
        } else if (instruction == 0x16fcu || instruction == 0x16fdu) {
            code << "    NativeDspWriteOutput(dsp, "
                 << Hex(static_cast<uint16_t>(0xff00u | (instruction & 0xffu)))
                 << ", " << Hex(word(pc + 1)) << ");\n";
            pc += 2;
        } else if (instruction == 0x0021u) {
            code << "    dsp.programHalted = true;\n"
                    "    ++dsp.completedPrograms;\n"
                    "    return;\n}\n";
            return code.str();
        } else {
            reject(pc);
        }
    }
    throw std::runtime_error("DSP initialization does not reach a compiled HALT.");
}
