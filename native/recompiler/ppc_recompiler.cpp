#include "ppc_recompiler.h"

#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace {

uint32_t Bits(uint32_t value, uint32_t shift, uint32_t width) {
    return (value >> shift) & ((1u << width) - 1u);
}

int32_t SignExtend(uint32_t value, uint32_t width) {
    const uint32_t signBit = 1u << (width - 1u);
    return static_cast<int32_t>((value ^ signBit) - signBit);
}

std::string Hex(uint32_t value) {
    std::ostringstream output;
    output << "0x" << std::hex << std::uppercase << std::setw(8)
           << std::setfill('0') << value;
    return output.str();
}

uint32_t RelativeTarget(uint32_t address, int32_t displacement, bool absolute) {
    return absolute ? static_cast<uint32_t>(displacement)
                    : address + static_cast<uint32_t>(displacement);
}

bool IsInFunction(uint32_t target, uint32_t address, uint64_t end) {
    return target >= address && static_cast<uint64_t>(target) < end &&
           ((target - address) & 3u) == 0;
}

std::string Label(uint32_t address) {
    return "loc_" + Hex(address).substr(2);
}

std::string EffectiveAddress(uint32_t base, int32_t displacement) {
    if (base == 0) {
        return "static_cast<uint32_t>(" + std::to_string(displacement) + ")";
    }
    return "state.gpr[" + std::to_string(base) +
           "] + static_cast<uint32_t>(" + std::to_string(displacement) + ")";
}

void EmitRecordResult(std::ostringstream& output, bool record,
                      const std::string& result) {
    if (record) {
        output << "    ppc_set_cr_field(state, 0, "
               << "static_cast<int32_t>(" << result << ") < 0, "
               << "static_cast<int32_t>(" << result << ") > 0, "
               << result << " == 0);\n";
    }
}

void EmitBranch(std::ostringstream& output, uint32_t target, uint32_t address,
                uint32_t next, uint32_t nextAddress, uint64_t end,
                bool conditional) {
    if (IsInFunction(target, address, end)) {
        output << "        goto " << Label(target) << ";\n";
    } else {
        output << "        state.pc = " << Hex(target) << ";\n"
               << "        return;\n";
    }
    if (conditional) {
        if (static_cast<uint64_t>(nextAddress) < end) {
            output << "    } else {\n        goto " << Label(nextAddress)
                   << ";\n    }\n";
        } else {
            output << "    } else {\n        state.pc = " << Hex(next)
                   << ";\n        return;\n    }\n";
        }
    }
}

} // namespace

std::string RecompileFunction(uint32_t address,
                              const std::vector<uint32_t>& instructions) {
    if (instructions.empty() || (address & 3u) != 0) {
        throw std::runtime_error(
            "Function requires a 4-byte-aligned address and at least one instruction.");
    }

    const uint64_t end = static_cast<uint64_t>(address) +
                         instructions.size() * sizeof(uint32_t);
    if (end > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error("Function address range overflows.");
    }

    std::ostringstream output;
    bool hasIndirectBranch = false;
    for (const auto instruction : instructions) {
        const uint32_t extendedOpcode = Bits(instruction, 1, 10);
        if (Bits(instruction, 26, 6) == 19 && (instruction & 1u) == 0 &&
            extendedOpcode == 528) {
            hasIndirectBranch = true;
            break;
        }
    }
    output << "static void ppc_" << Hex(address).substr(2)
           << "(PpcState& state) {\n";
    if (hasIndirectBranch) {
        output << "    uint32_t indirect_target = 0;\n";
    }
    output << "    goto " << Label(address) << ";\n";

    for (size_t index = 0; index < instructions.size(); ++index) {
        const uint32_t pc = address + static_cast<uint32_t>(index * 4);
        const uint32_t next = pc + 4;
        const uint32_t instruction = instructions[index];
        const uint32_t opcode = Bits(instruction, 26, 6);
        const uint32_t rt = Bits(instruction, 21, 5);
        const uint32_t ra = Bits(instruction, 16, 5);
        const uint32_t rb = Bits(instruction, 11, 5);
        const uint32_t immediate = Bits(instruction, 0, 16);

        output << "\n" << Label(pc) << ": {\n"
               << "    // " << Hex(pc) << ": " << Hex(instruction) << "\n";

        switch (opcode) {
        case 10:
        case 11: {
            const uint32_t field = Bits(instruction, 23, 3);
            const int32_t value = SignExtend(immediate, 16);
            output << "    ppc_set_cr_field(state, " << field << ", ";
            if (opcode == 11) {
                output << "static_cast<int32_t>(state.gpr[" << ra
                       << "]) < " << value << ", "
                       << "static_cast<int32_t>(state.gpr[" << ra
                       << "]) > " << value << ", state.gpr[" << ra << "] == "
                       << value;
            } else {
                output << "state.gpr[" << ra << "] < " << immediate << ", "
                       << "state.gpr[" << ra << "] > " << immediate << ", "
                       << "state.gpr[" << ra << "] == " << immediate;
            }
            output << ");\n";
            break;
        }
        case 14:
        case 15: {
            const int32_t value = SignExtend(immediate, 16);
            const int64_t shifted =
                opcode == 15 ? static_cast<int64_t>(value) * 65536 : value;
            output << "    state.gpr[" << rt << "] = "
                   << (ra == 0 ? "0u" : "state.gpr[" + std::to_string(ra) + "]")
                   << " + static_cast<uint32_t>(" << shifted << ");\n";
            break;
        }
        case 8:
            output << "    state.gpr[" << rt
                   << "] = ppc_subtract_carry(state, state.gpr[" << ra
                   << "], static_cast<uint32_t>(" << SignExtend(immediate, 16) << "));\n";
            break;
        case 7: {
            const int32_t value = SignExtend(immediate, 16);
            output << "    state.gpr[" << rt
                   << "] = static_cast<uint32_t>(static_cast<int64_t>("
                      "static_cast<int32_t>(state.gpr["
                   << ra << "])) * " << value << "ll);\n";
            break;
        }
        case 12:
        case 13: {
            const int32_t value = SignExtend(immediate, 16);
            output << "    const uint32_t result_" << Hex(pc).substr(2)
                   << " = ppc_add_carry(state, state.gpr[" << ra
                   << "], static_cast<uint32_t>(" << value << "));\n"
                   << "    state.gpr[" << rt << "] = result_"
                   << Hex(pc).substr(2) << ";\n";
            if (opcode == 13) {
                output << "    ppc_set_cr_field(state, 0, "
                       << "static_cast<int32_t>(result_" << Hex(pc).substr(2)
                       << ") < 0, "
                       << "static_cast<int32_t>(result_" << Hex(pc).substr(2)
                       << ") > 0, result_" << Hex(pc).substr(2)
                       << " == 0);\n";
            }
            break;
        }
        case 16: {
            const uint32_t bo = Bits(instruction, 21, 5);
            const uint32_t bi = Bits(instruction, 16, 5);
            const int32_t displacement =
                SignExtend(instruction & 0x0000fffcu, 16);
            const uint32_t target =
                RelativeTarget(pc, displacement, (instruction & 2u) != 0);
            if ((instruction & 1u) != 0) {
                output << "    state.lr = " << Hex(next) << ";\n";
            }
            output << "    if (ppc_branch_condition(state, " << bo << ", "
                   << bi << ")) {\n";
            if ((instruction & 1u) != 0) {
                output << "        ppc_call_guest(state, " << Hex(target) << ");\n"
                       << "    }\n";
                if (static_cast<uint64_t>(next) < end) {
                    output << "    goto " << Label(next) << ";\n";
                } else {
                    output << "    state.pc = " << Hex(next) << ";\n"
                           << "    return;\n";
                }
            } else {
                EmitBranch(output, target, address, next, next, end, true);
            }
            break;
        }
        case 18: {
            const int32_t displacement =
                SignExtend(instruction & 0x03fffffcu, 26);
            const uint32_t target =
                RelativeTarget(pc, displacement, (instruction & 2u) != 0);
            if ((instruction & 1u) != 0) {
                output << "    state.lr = " << Hex(next) << ";\n"
                       << "    ppc_call_guest(state, " << Hex(target) << ");\n";
                if (static_cast<uint64_t>(next) < end) {
                    output << "    goto " << Label(next) << ";\n";
                } else {
                    output << "    state.pc = " << Hex(next) << ";\n"
                           << "    return;\n";
                }
            } else {
                EmitBranch(output, target, address, next, next, end, false);
            }
            break;
        }
        case 24:
        case 25:
        case 26:
        case 27:
        case 28:
        case 29: {
            const uint32_t source = rt;
            const uint32_t shift = (opcode == 25 || opcode == 27) ? 16 : 0;
            const char* operation =
                (opcode == 26 || opcode == 27) ? "^" :
                (opcode == 28 || opcode == 29) ? "&" : "|";
            const uint32_t value = immediate << shift;
            output << "    state.gpr[" << ra << "] = state.gpr[" << source
                   << "] " << operation << " " << Hex(value) << ";\n";
            if (opcode == 28 || opcode == 29) {
                EmitRecordResult(output, true,
                                 "state.gpr[" + std::to_string(ra) + "]");
            }
            break;
        }
        case 20:
        case 21:
        case 23: {
            const uint32_t shift =
                opcode == 23 ? 0u : Bits(instruction, 11, 5);
            const uint32_t begin = Bits(instruction, 6, 5);
            const uint32_t finish = Bits(instruction, 1, 5);
            const std::string shiftExpression =
                opcode == 23 ? "(state.gpr[" + std::to_string(rb) +
                                   "] & 31u)"
                             : std::to_string(shift) + "u";
            const std::string rotated =
                "ppc_rotate_left32(state.gpr[" + std::to_string(rt) +
                "], " + shiftExpression + ")";
            const std::string mask =
                "ppc_mask32(" + std::to_string(begin) + "u, " +
                std::to_string(finish) + "u)";
            if (opcode == 20) {
                output << "    state.gpr[" << ra << "] = (state.gpr[" << ra
                       << "] & ~" << mask << ") | (" << rotated << " & "
                       << mask << ");\n";
            } else {
                output << "    state.gpr[" << ra << "] = " << rotated << " & "
                       << mask << ";\n";
            }
            EmitRecordResult(output, (instruction & 1u) != 0,
                             "state.gpr[" + std::to_string(ra) + "]");
            break;
        }
        case 32:
        case 33:
        case 34:
        case 35:
        case 40:
        case 41:
        case 42:
        case 43:
        case 36:
        case 37:
        case 38:
        case 39:
        case 44:
        case 45:
        case 46:
        case 47: {
            const int32_t displacement = SignExtend(immediate, 16);
            const std::string ea =
                EffectiveAddress(ra == 0 ? 0 : ra, displacement);
            const bool update = opcode == 33 || opcode == 35 ||
                                opcode == 37 || opcode == 39 ||
                                opcode == 41 || opcode == 43 ||
                                opcode == 45;
            const bool load = opcode == 32 || opcode == 33 ||
                              opcode == 34 || opcode == 35 ||
                              opcode == 40 || opcode == 41 ||
                              opcode == 42 || opcode == 43 ||
                              opcode == 46;
            const bool multi = opcode == 46 || opcode == 47;
            if (update && (ra == 0 || (load && !multi && rt == ra))) {
                throw std::runtime_error("Invalid update-form PowerPC instruction " +
                                         Hex(instruction) + " at " + Hex(pc) +
                                         ".");
            }
            if (update || multi) {
                output << "    const uint32_t ea_" << Hex(pc).substr(2)
                       << " = " << ea << ";\n";
            }
            const std::string effectiveAddress =
                (update || multi) ? "ea_" + Hex(pc).substr(2) : ea;
            if (multi) {
                output << "    for (uint32_t reg = " << rt
                       << "; reg < 32; ++reg) {\n";
                if (opcode == 46) {
                    output << "        state.gpr[reg] = ppc_read_u32(state, "
                           << effectiveAddress << " + (reg - " << rt
                           << ") * 4u);\n";
                } else {
                    output << "        ppc_write_u32(state, " << effectiveAddress
                           << " + (reg - " << rt << ") * 4u, state.gpr[reg]);\n";
                }
                output << "    }\n";
            } else if (opcode == 32 || opcode == 33) {
                output << "    state.gpr[" << rt << "] = ppc_read_u32(state, "
                       << effectiveAddress << ");\n";
            } else if (opcode == 34 || opcode == 35) {
                output << "    state.gpr[" << rt << "] = ppc_read_u8(state, "
                       << effectiveAddress << ");\n";
            } else if (opcode == 36 || opcode == 37) {
                output << "    ppc_write_u32(state, " << effectiveAddress
                       << ", state.gpr[" << rt << "]);\n";
            } else if (opcode == 38 || opcode == 39) {
                output << "    ppc_write_u8(state, " << effectiveAddress
                       << ", static_cast<uint8_t>(state.gpr[" << rt
                       << "]));\n";
            } else if (opcode == 40 || opcode == 41) {
                output << "    state.gpr[" << rt << "] = "
                       << "static_cast<uint32_t>(ppc_read_u16(state, "
                       << effectiveAddress << "));\n";
            } else if (opcode == 42 || opcode == 43) {
                output << "    state.gpr[" << rt
                       << "] = ppc_sign_extend16(ppc_read_u16(state, "
                       << effectiveAddress << "));\n";
            } else if (opcode == 44 || opcode == 45) {
                output << "    ppc_write_u16(state, " << effectiveAddress
                       << ", static_cast<uint16_t>(state.gpr[" << rt
                       << "]));\n";
            }
            if (update) {
                output << "    state.gpr[" << ra << "] = " << effectiveAddress
                       << ";\n";
            }
            break;
        }
        case 19: {
            const uint32_t extendedOpcode = Bits(instruction, 1, 10);
            const uint32_t bo = Bits(instruction, 21, 5);
            const uint32_t bi = Bits(instruction, 16, 5);
            const uint32_t link = instruction & 1u;
            if (extendedOpcode == 193) {
                if (link != 0) {
                    throw std::runtime_error("Invalid crxor instruction at " + Hex(pc) + ".");
                }
                output << "    const uint32_t cr_bit = ((state.cr >> " << (31u - ra)
                       << ") ^ (state.cr >> " << (31u - rb) << ")) & 1u;\n"
                       << "    state.cr = (state.cr & ~" << Hex(1u << (31u - rt))
                       << ") | (cr_bit << " << (31u - rt) << ");\n";
                break;
            }
            if (rb != 0 || (extendedOpcode != 16 && extendedOpcode != 528)) {
                throw std::runtime_error("Unsupported PowerPC instruction " +
                                         Hex(instruction) + " at " + Hex(pc) +
                                         ".");
            }
            if (extendedOpcode == 528 && (bo & 0x04u) == 0) {
                throw std::runtime_error(
                    "bcctr cannot test/decrement CTR at " + Hex(pc) + ".");
            }

            const std::string target = "target_" + Hex(pc).substr(2);
            output << "    const uint32_t " << target << " = state."
                   << (extendedOpcode == 16 ? "lr" : "ctr") << " & ~3u;\n";
            if (link != 0) {
                output << "    state.lr = " << Hex(next) << ";\n";
            }
            output << "    if (ppc_branch_condition(state, " << bo << ", " << bi
                   << ")) {\n";
            if (link != 0) {
                output << "        ppc_call_guest(state, " << target << ");\n";
                if (static_cast<uint64_t>(next) < end) {
                    output << "        goto " << Label(next) << ";\n";
                } else {
                    output << "        state.pc = " << Hex(next) << ";\n"
                           << "        return;\n";
                }
            } else if (extendedOpcode == 528) {
                output << "        indirect_target = " << target << ";\n"
                       << "        goto indirect_branch;\n";
            } else {
                output << "        state.pc = " << target << ";\n"
                       << "        return;\n";
            }
            if (static_cast<uint64_t>(next) < end) {
                output << "    } else {\n        goto " << Label(next)
                       << ";\n    }\n";
            } else {
                output << "    } else {\n        state.pc = " << Hex(next)
                       << ";\n        return;\n    }\n";
            }
            break;
        }
        case 31: {
            const uint32_t extendedOpcode = Bits(instruction, 1, 10);
            const std::string left = "state.gpr[" + std::to_string(ra) + "]";
            const std::string right = "state.gpr[" + std::to_string(rb) + "]";
            std::string expression;
            bool emitted = false;
            uint32_t destination = rt;
            bool record = (instruction & 1u) != 0;
            switch (extendedOpcode) {
            case 26:
                if (rb != 0) {
                    throw std::runtime_error("Invalid cntlzw instruction at " + Hex(pc) + ".");
                }
                destination = ra;
                expression = "ppc_count_leading_zeros32(state.gpr[" +
                             std::to_string(rt) + "])";
                break;
            case 922:
                if (rb != 0) {
                    throw std::runtime_error("Invalid extsh instruction at " + Hex(pc) + ".");
                }
                destination = ra;
                expression = "ppc_sign_extend16(static_cast<uint16_t>(state.gpr[" +
                             std::to_string(rt) + "]))";
                break;
            case 954:
                if (rb != 0) {
                    throw std::runtime_error("Invalid extsb instruction at " + Hex(pc) + ".");
                }
                destination = ra;
                expression = "((state.gpr[" + std::to_string(rt) +
                    "] & 0x80u) != 0 ? state.gpr[" + std::to_string(rt) +
                    "] | 0xffffff00u : state.gpr[" + std::to_string(rt) + "] & 0xffu)";
                break;
            case 104:
                if (rb != 0) {
                    throw std::runtime_error("Invalid neg instruction at " + Hex(pc) + ".");
                }
                expression = "0u - " + left;
                break;
            case 0:
            case 32: {
                if (record || (rt & 3u) != 0) {
                    throw std::runtime_error(
                        "Invalid or unsupported 64-bit PowerPC comparison at " +
                        Hex(pc) + ".");
                }
                const auto operand = [extendedOpcode](const std::string& value) {
                    return extendedOpcode == 0
                        ? "static_cast<int32_t>(" + value + ")" : value;
                };
                output << "    ppc_set_cr_field(state, " << (rt >> 2) << ", "
                       << operand(left) << " < " << operand(right) << ", "
                       << operand(left) << " > " << operand(right) << ", "
                       << left << " == " << right << ");\n";
                emitted = true;
                break;
            }
            case 24:
                destination = ra;
                expression = "(" + right + " & 63u) >= 32u ? 0u : state.gpr[" +
                             std::to_string(rt) + "] << (" + right + " & 31u)";
                break;
            case 28:
                destination = ra;
                expression = "state.gpr[" + std::to_string(rt) + "] & " + right;
                break;
            case 23:
            case 55:
            case 87:
            case 119:
            case 279:
            case 311:
            case 343:
            case 375:
            case 151:
            case 183:
            case 215:
            case 247:
            case 407:
            case 439: {
                const bool update =
                    extendedOpcode == 55 || extendedOpcode == 119 ||
                    extendedOpcode == 311 || extendedOpcode == 375 ||
                    extendedOpcode == 183 || extendedOpcode == 247 ||
                    extendedOpcode == 439;
                const bool load =
                    extendedOpcode == 23 || extendedOpcode == 55 ||
                    extendedOpcode == 87 || extendedOpcode == 119 ||
                    extendedOpcode == 279 || extendedOpcode == 311 ||
                    extendedOpcode == 343 || extendedOpcode == 375;
                const bool word =
                    extendedOpcode == 23 || extendedOpcode == 55 ||
                    extendedOpcode == 151 || extendedOpcode == 183;
                const bool half =
                    extendedOpcode == 279 || extendedOpcode == 311 ||
                    extendedOpcode == 343 || extendedOpcode == 375 ||
                    extendedOpcode == 407 || extendedOpcode == 439;
                const bool signedHalf =
                    extendedOpcode == 343 || extendedOpcode == 375;
                const bool byte = !word && !half;
                if (update && (ra == 0 || (load && rt == ra))) {
                    throw std::runtime_error(
                        "Invalid indexed update-form instruction " +
                        Hex(instruction) + " at " + Hex(pc) + ".");
                }
                const std::string eaName = "ea_" + Hex(pc).substr(2);
                const std::string base =
                    ra == 0 ? "0u" : "state.gpr[" + std::to_string(ra) + "]";
                output << "    const uint32_t " << eaName << " = " << base
                       << " + state.gpr[" << rb << "];\n";
                if (load) {
                    output << "    state.gpr[" << rt << "] = ";
                    if (signedHalf) {
                        output << "ppc_sign_extend16(";
                    } else if (half) {
                        output << "static_cast<uint32_t>(";
                    } else if (byte) {
                        output << "static_cast<uint32_t>(";
                    }
                    output << (word ? "ppc_read_u32(state, " :
                              half ? "ppc_read_u16(state, " :
                                     "ppc_read_u8(state, ")
                           << eaName << ")";
                    if (!word) {
                        output << ")";
                    }
                    output << ";\n";
                } else {
                    output << (word ? "    ppc_write_u32(state, " :
                              half ? "    ppc_write_u16(state, " :
                                     "    ppc_write_u8(state, ")
                           << eaName << ", ";
                    if (half) {
                        output << "static_cast<uint16_t>(";
                    } else if (byte) {
                        output << "static_cast<uint8_t>(";
                    }
                    output << "state.gpr[" << rt << "]";
                    if (!word) {
                        output << ")";
                    }
                    output << ");\n";
                }
                if (update) {
                    output << "    state.gpr[" << ra << "] = " << eaName
                           << ";\n";
                }
                emitted = true;
                break;
            }
            case 40:
                expression = right + " - " + left;
                break;
            case 136:
                expression = "ppc_subtract_extended(state, " + left + ", " + right + ")";
                break;
            case 266:
                expression = left + " + " + right;
                break;
            case 235:
                expression = "static_cast<uint32_t>(static_cast<uint64_t>(" +
                             left + ") * " + right + ")";
                break;
            case 459:
                expression = "ppc_divide_unsigned32(" + left + ", " + right + ")";
                break;
            case 444:
                destination = ra;
                expression = "state.gpr[" + std::to_string(rt) + "] | " + right;
                break;
            case 124:
                destination = ra;
                expression = "~(state.gpr[" + std::to_string(rt) + "] | " + right + ")";
                break;
            case 536:
                destination = ra;
                expression = "(" + right + " & 63u) >= 32u ? 0u : state.gpr[" +
                             std::to_string(rt) + "] >> (" + right + " & 31u)";
                break;
            case 339: {
                const uint32_t spr = ra | (rb << 5);
                if (record || (spr != 1 && spr != 8 && spr != 9)) {
                    throw std::runtime_error("Unsupported mfspr instruction " +
                                             Hex(instruction) + " at " +
                                             Hex(pc) + ".");
                }
                expression = spr == 1 ? "state.xer" : spr == 8 ? "state.lr" : "state.ctr";
                record = false;
                break;
            }
            case 371: {
                const uint32_t tbr = ra | (rb << 5);
                if (record || (tbr != 268 && tbr != 269)) {
                    throw std::runtime_error("Invalid or unsupported mftb instruction at " + Hex(pc) + ".");
                }
                expression = tbr == 268 ?
                    "static_cast<uint32_t>(ppc_read_time_base(state))" :
                    "static_cast<uint32_t>(ppc_read_time_base(state) >> 32)";
                break;
            }
            case 467: {
                const uint32_t spr = ra | (rb << 5);
                if (record || (spr != 1 && spr != 8 && spr != 9)) {
                    throw std::runtime_error("Unsupported mtspr instruction " +
                                             Hex(instruction) + " at " +
                                             Hex(pc) + ".");
                }
                output << "    state."
                       << (spr == 1 ? "xer" : spr == 8 ? "lr" : "ctr")
                       << " = state.gpr[" << rt << "];\n";
                emitted = true;
                break;
            }
            default:
                throw std::runtime_error("Unsupported PowerPC instruction " +
                                         Hex(instruction) + " at " + Hex(pc) +
                                         ".");
            }
            if (!emitted) {
                output << "    state.gpr[" << destination << "] = " << expression << ";\n";
                EmitRecordResult(output, record,
                                 "state.gpr[" + std::to_string(destination) + "]");
            }
            break;
        }
        default:
            throw std::runtime_error("Unsupported PowerPC instruction " +
                                     Hex(instruction) + " at " + Hex(pc) +
                                     ".");
        }

        if (opcode != 16 && opcode != 18 && opcode != 19 &&
            static_cast<uint64_t>(next) < end) {
            output << "    goto " << Label(next) << ";\n";
        } else if (opcode != 16 && opcode != 18 && opcode != 19) {
            output << "    state.pc = " << Hex(next) << ";\n"
                   << "    return;\n";
        }
        output << "}\n";
    }
    if (hasIndirectBranch) {
        output << "\nindirect_branch:\n    switch (indirect_target) {\n";
        for (size_t local = 0; local < instructions.size(); ++local) {
            const uint32_t destination = address + static_cast<uint32_t>(local * 4);
            output << "    case " << Hex(destination) << ": goto "
                   << Label(destination) << ";\n";
        }
        output << "    default: state.pc = indirect_target; return;\n"
               << "    }\n";
    }
    output << "}\n";
    return output.str();
}
