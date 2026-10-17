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

void EmitFloatingTransfer(std::ostringstream& output, uint32_t pc, uint32_t reg,
                        uint32_t base, const std::string& address, bool load,
                        bool update, bool single = false) {
    if (update && base == 0) {
        throw std::runtime_error("Invalid floating-point update-form instruction at " +
                                 Hex(pc) + ".");
    }
    const std::string ea = "ea_" + Hex(pc).substr(2);
    output << "    const uint32_t " << ea << " = " << address << ";\n"
           << "    if ((" << ea << " & 3u) != 0) {\n"
           << "        throw std::runtime_error(\"Unaligned PowerPC floating-point transfer at "
           << Hex(pc) << ".\");\n"
           << "    }\n";
    if (load && single) {
        output << "    const uint64_t value = ppc_widen_single_bits(ppc_read_u32(state, "
               << ea << "));\n"
               << "    state.fpr[" << reg << "] = {value, value};\n";
    } else if (load) {
        output << "    state.fpr[" << reg << "].ps0 = ppc_read_u64(state, "
               << ea << ");\n";
    } else if (single) {
        output << "    ppc_write_u32(state, " << ea << ", ppc_narrow_single_bits(state.fpr["
               << reg << "].ps0));\n";
    } else {
        output << "    ppc_write_u64(state, " << ea << ", state.fpr[" << reg
               << "].ps0);\n";
    }
    if (update) {
        output << "    state.gpr[" << base << "] = " << ea << ";\n";
    }
}

void EmitFpscrRecord(std::ostringstream& output, bool record) {
    if (record) {
        output << "    state.cr = (state.cr & 0xf0ffffffu) | "
                  "((state.fpscr >> 4) & 0x0f000000u);\n";
    }
}

void EmitPairedTransfer(std::ostringstream& output, uint32_t pc, uint32_t reg,
                    uint32_t base, const std::string& address, uint32_t selector,
                    bool single, bool update, bool load = true) {
    if (update && base == 0) {
        throw std::runtime_error("Invalid paired transfer update-form instruction at " + Hex(pc) + ".");
    }
    output << "    const uint32_t ea = " << address << ";\n";
    if (load) {
        output << "    state.fpr[" << reg << "] = ppc_load_quantized_pair(state, ea, "
               << selector << "u, " << (single ? "true" : "false") << ");\n";
    } else {
        output << "    ppc_store_float_pair(state, ea, state.fpr[" << reg << "], "
               << selector << "u, " << (single ? "true" : "false") << ");\n";
    }
    if (update) {
        output << "    state.gpr[" << base << "] = ea;\n";
    }
}

void EmitBranch(std::ostringstream& output, uint32_t target, uint32_t address,
                uint32_t next, uint32_t nextAddress, uint64_t end,
                bool conditional, bool yieldAtBranches) {
    if (!yieldAtBranches && IsInFunction(target, address, end)) {
        output << "        goto " << Label(target) << ";\n";
    } else {
        output << "        state.pc = " << Hex(target) << ";\n"
               << "        return;\n";
    }
    if (conditional) {
        if (!yieldAtBranches && static_cast<uint64_t>(nextAddress) < end) {
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
                              const std::vector<uint32_t>& instructions,
                              RecompileOptions options) {
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
        if (!options.yieldAtBranches &&
            Bits(instruction, 26, 6) == 19 && (instruction & 1u) == 0 &&
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
        if (options.precisePc) {
            output << "    state.pc = " << Hex(pc) << ";\n";
        }

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
                if (!options.yieldAtBranches && static_cast<uint64_t>(next) < end) {
                    output << "    goto " << Label(next) << ";\n";
                } else {
                    output << "    state.pc = " << Hex(next) << ";\n"
                           << "    return;\n";
                }
            } else {
                EmitBranch(output, target, address, next, next, end, true,
                           options.yieldAtBranches);
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
                if (!options.yieldAtBranches && static_cast<uint64_t>(next) < end) {
                    output << "    goto " << Label(next) << ";\n";
                } else {
                    output << "    state.pc = " << Hex(next) << ";\n"
                           << "    return;\n";
                }
            } else {
                EmitBranch(output, target, address, next, next, end, false,
                           options.yieldAtBranches);
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
        case 56:
        case 57:
        case 60:
        case 61: {
            EmitPairedTransfer(output, pc, rt, ra,
                EffectiveAddress(ra, SignExtend(Bits(instruction, 0, 12), 12)),
                Bits(instruction, 12, 3), Bits(instruction, 15, 1) != 0,
                opcode == 57 || opcode == 61, opcode < 60);
            break;
        }
        case 4: {
            const uint32_t extendedOpcode = Bits(instruction, 1, 10);
            const uint32_t arithmetic = Bits(instruction, 1, 5);
            if (arithmetic == 10 || arithmetic == 11) {
                output << "    PpcPairedSum(state, " << rt << ", " << ra << ", " << rb << ", "
                       << Bits(instruction, 6, 5) << ", "
                       << (arithmetic == 11 ? "true" : "false") << ");\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (arithmetic >= 28 && arithmetic <= 31) {
                output << "    PpcFloatFused(state, " << rt << ", " << ra << ", "
                       << Bits(instruction, 6, 5) << ", " << rb << ", "
                       << ((arithmetic & 1u) == 0 ? "true" : "false") << ", true, true, "
                       << (arithmetic >= 30 ? "true" : "false") << ");\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (arithmetic == 18 || arithmetic == 20 || arithmetic == 21 ||
                arithmetic == 25 || arithmetic == 12 || arithmetic == 13) {
                const auto rc = Bits(instruction, 6, 5);
                const bool multiply = arithmetic == 25 || arithmetic == 12 || arithmetic == 13;
                if ((multiply && rb != 0) || (!multiply && rc != 0)) {
                    throw std::runtime_error("Invalid paired arithmetic encoding at " + Hex(pc) + ".");
                }
                output << "    PpcPairedArithmetic(state, " << rt << ", " << ra << ", "
                       << (multiply ? rc : rb) << ", PpcFloatOperation::"
                       << (arithmetic == 18 ? "Divide" : arithmetic == 20 ? "Subtract" :
                           arithmetic == 21 ? "Add" : "Multiply")
                       << ", " << (arithmetic == 12 ? 0 : arithmetic == 13 ? 1 : -1) << ");\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            const uint32_t quantizedOpcode = Bits(instruction, 1, 6);
            if (quantizedOpcode == 6 || quantizedOpcode == 38 ||
                quantizedOpcode == 7 || quantizedOpcode == 39) {
                if ((instruction & 1u) != 0) {
                    throw std::runtime_error("Invalid indexed paired transfer at " + Hex(pc) + ".");
                }
                const std::string base =
                    ra == 0 ? "0u" : "state.gpr[" + std::to_string(ra) + "]";
                EmitPairedTransfer(output, pc, rt, ra,
                    base + " + state.gpr[" + std::to_string(rb) + "]",
                    Bits(instruction, 7, 3), Bits(instruction, 10, 1) != 0,
                    quantizedOpcode >= 38, (quantizedOpcode & 1u) == 0);
                break;
            }
            if (extendedOpcode == 528 || extendedOpcode == 560 ||
                extendedOpcode == 592 || extendedOpcode == 624) {
                const unsigned first = (extendedOpcode - 528) / 64;
                const unsigned second = ((extendedOpcode - 528) / 32) % 2;
                output << "    const uint64_t first = state.fpr[" << ra << "].ps"
                       << first << ";\n"
                       << "    const uint64_t second = state.fpr[" << rb << "].ps"
                       << second << ";\n"
                       << "    state.fpr[" << rt << "] = {first, second};\n";
            } else {
                if (ra != 0 || (extendedOpcode != 40 && extendedOpcode != 72 &&
                                extendedOpcode != 136 && extendedOpcode != 264)) {
                    throw UnsupportedPpcInstruction("Unsupported PowerPC instruction " +
                                             Hex(instruction) + " at " + Hex(pc) + ".");
                }
                for (unsigned lane = 0; lane < 2; ++lane) {
                    output << "    state.fpr[" << rt << "].ps" << lane
                           << " = state.fpr[" << rb << "].ps" << lane;
                    if (extendedOpcode == 40) {
                        output << " ^ UINT64_C(0x8000000000000000)";
                    } else if (extendedOpcode == 136) {
                        output << " | UINT64_C(0x8000000000000000)";
                    } else if (extendedOpcode == 264) {
                        output << " & UINT64_C(0x7fffffffffffffff)";
                    }
                    output << ";\n";
                }
            }
            EmitFpscrRecord(output, (instruction & 1u) != 0);
            break;
        }
        case 48:
        case 49:
        case 50:
        case 51:
        case 52:
        case 53:
        case 54:
        case 55:
            EmitFloatingTransfer(output, pc, rt, ra,
                EffectiveAddress(ra, SignExtend(immediate, 16)),
                opcode <= 51, opcode == 49 || opcode == 51 || opcode == 53 || opcode == 55,
                opcode == 48 || opcode == 49 || opcode == 52 || opcode == 53);
            break;
        case 59:
        case 63: {
            const uint32_t extendedOpcode = Bits(instruction, 1, 10);
            const uint32_t arithmetic = Bits(instruction, 1, 5);
            if (arithmetic >= 28 && arithmetic <= 31) {
                output << "    PpcFloatFused(state, " << rt << ", " << ra << ", "
                       << Bits(instruction, 6, 5) << ", " << rb << ", "
                       << ((arithmetic & 1u) == 0 ? "true" : "false") << ", "
                       << (opcode == 59 ? "true" : "false") << ", false, "
                       << (arithmetic >= 30 ? "true" : "false") << ");\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (arithmetic == 18 || arithmetic == 20 || arithmetic == 21 || arithmetic == 25) {
                const auto rc = Bits(instruction, 6, 5);
                if ((arithmetic == 25 && rb != 0) || (arithmetic != 25 && rc != 0)) {
                    throw std::runtime_error("Invalid floating-point arithmetic encoding at " + Hex(pc) + ".");
                }
                output << "    PpcFloatArithmetic(state, " << rt << ", " << ra << ", "
                       << (arithmetic == 25 ? rc : rb) << ", PpcFloatOperation::"
                       << (arithmetic == 18 ? "Divide" : arithmetic == 20 ? "Subtract" :
                           arithmetic == 21 ? "Add" : "Multiply")
                       << ", " << (opcode == 59 ? "true" : "false") << ");\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (opcode == 59) {
                throw UnsupportedPpcInstruction("Unsupported single-precision arithmetic at " + Hex(pc) + ".");
            }
            if (extendedOpcode == 26 || extendedOpcode == 12) {
                if (ra != 0) {
                    throw std::runtime_error("Invalid frsqrte/frsp encoding at " + Hex(pc) + ".");
                }
                output << "    " << (extendedOpcode == 26 ? "PpcFloatReciprocalSqrtEstimate" : "PpcFloatRoundSingle")
                       << "(state, " << rt << ", " << rb << ");\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (extendedOpcode == 0 || extendedOpcode == 32) {
                if ((instruction & 0x00600001u) != 0) {
                    throw std::runtime_error("Invalid floating-point comparison at " + Hex(pc) + ".");
                }
                output << "    PpcFloatCompare(state, " << (rt >> 2) << ", " << ra << ", " << rb
                       << ", " << (extendedOpcode == 32 ? "true" : "false") << ");\n";
                break;
            }
            if (extendedOpcode == 15) {
                if (ra != 0) {
                    throw std::runtime_error("Invalid fctiwz instruction at " + Hex(pc) + ".");
                }
                output << "    PpcFloatToInteger(state, " << rt << ", " << rb << ");\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (extendedOpcode == 38 || extendedOpcode == 70) {
                if (ra != 0 || rb != 0) {
                    throw std::runtime_error("Invalid FPSCR bit instruction at " + Hex(pc) + ".");
                }
                output << "    ppc_write_fpscr_bit(state, " << Hex(0x80000000u >> rt)
                       << "u, " << (extendedOpcode == 38 ? "true" : "false") << ");\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (extendedOpcode == 583) {
                if (ra != 0 || rb != 0) {
                    throw std::runtime_error("Invalid mffs instruction at " + Hex(pc) + ".");
                }
                output << "    state.fpr[" << rt
                       << "].ps0 = UINT64_C(0xfff8000000000000) | state.fpscr;\n";
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (extendedOpcode == 711 || extendedOpcode == 134) {
                uint32_t mask = 0;
                if (extendedOpcode == 711) {
                    if ((instruction & 0x02010000u) != 0) {
                        throw std::runtime_error("Invalid mtfsf instruction at " + Hex(pc) + ".");
                    }
                    const uint32_t fields = Bits(instruction, 17, 8);
                    for (unsigned field = 0; field < 8; ++field) {
                        if ((fields & (1u << field)) != 0) {
                            mask |= 0xfu << (field * 4u);
                        }
                    }
                    output << "    ppc_write_fpscr(state, static_cast<uint32_t>(state.fpr["
                           << rb << "].ps0), " << Hex(mask) << "u);\n";
                } else {
                    if ((instruction & 0x007f0800u) != 0) {
                        throw std::runtime_error("Invalid mtfsfi instruction at " + Hex(pc) + ".");
                    }
                    const uint32_t shift = 28u - Bits(instruction, 23, 3) * 4u;
                    mask = 0xfu << shift;
                    output << "    ppc_write_fpscr(state, "
                           << Hex(Bits(instruction, 12, 4) << shift) << "u, "
                           << Hex(mask) << "u);\n";
                }
                EmitFpscrRecord(output, (instruction & 1u) != 0);
                break;
            }
            if (ra != 0 || (extendedOpcode != 40 && extendedOpcode != 72 &&
                            extendedOpcode != 136 && extendedOpcode != 264)) {
                throw UnsupportedPpcInstruction("Unsupported PowerPC instruction " +
                                         Hex(instruction) + " at " + Hex(pc) + ".");
            }
            output << "    state.fpr[" << rt << "].ps0 = state.fpr[" << rb << "].ps0";
            if (extendedOpcode == 40) {
                output << " ^ UINT64_C(0x8000000000000000)";
            } else if (extendedOpcode == 136) {
                output << " | UINT64_C(0x8000000000000000)";
            } else if (extendedOpcode == 264) {
                output << " & UINT64_C(0x7fffffffffffffff)";
            }
            output << ";\n";
            EmitFpscrRecord(output, (instruction & 1u) != 0);
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
        case 17:
            if (instruction != 0x44000002u) {
                throw std::runtime_error("Unsupported sc encoding at " + Hex(pc) + ".");
            }
            output << "    ppc_system_call(state, " << Hex(next) << "u);\n"
                   << "    state.pc = " << Hex(next) << "u;\n";
            if (!options.yieldAtBranches && static_cast<uint64_t>(next) < end) {
                output << "    goto " << Label(next) << ";\n";
            } else {
                output << "    return;\n";
            }
            break;
        case 19: {
            const uint32_t extendedOpcode = Bits(instruction, 1, 10);
            const uint32_t bo = Bits(instruction, 21, 5);
            const uint32_t bi = Bits(instruction, 16, 5);
            const uint32_t link = instruction & 1u;
            if (extendedOpcode == 50) {
                if (instruction != 0x4c000064u) {
                    throw std::runtime_error("Invalid rfi instruction at " + Hex(pc) + ".");
                }
                output << "    ppc_return_from_system_call(state);\n    return;\n";
                break;
            }
            if (extendedOpcode == 150) {
                if (instruction != 0x4c00012cu) {
                    throw std::runtime_error("Invalid isync instruction at " + Hex(pc) + ".");
                }
                output << "    ppc_sync();\n";
                break;
            }
            if (extendedOpcode == 193 || extendedOpcode == 449) {
                if (link != 0) {
                    throw std::runtime_error("Invalid CR logical instruction at " + Hex(pc) + ".");
                }
                output << "    const uint32_t cr_bit = ((state.cr >> " << (31u - ra)
                       << (extendedOpcode == 193 ? ") ^ (state.cr >> " : ") | (state.cr >> ")
                       << (31u - rb) << ")) & 1u;\n"
                       << "    state.cr = (state.cr & ~" << Hex(1u << (31u - rt))
                       << ") | (cr_bit << " << (31u - rt) << ");\n";
                break;
            }
            if (rb != 0 || (extendedOpcode != 16 && extendedOpcode != 528)) {
                throw UnsupportedPpcInstruction("Unsupported PowerPC instruction " +
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
                if (!options.yieldAtBranches && static_cast<uint64_t>(next) < end) {
                    output << "        goto " << Label(next) << ";\n";
                } else {
                    output << "        state.pc = " << Hex(next) << ";\n"
                           << "        return;\n";
                }
            } else if (extendedOpcode == 528 && !options.yieldAtBranches) {
                output << "        indirect_target = " << target << ";\n"
                       << "        goto indirect_branch;\n";
            } else {
                output << "        state.pc = " << target << ";\n"
                       << "        return;\n";
            }
            if (!options.yieldAtBranches && static_cast<uint64_t>(next) < end) {
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
            case 19:
                if (record || ra != 0 || rb != 0) {
                    throw std::runtime_error("Invalid mfcr instruction at " + Hex(pc) + ".");
                }
                expression = "state.cr";
                break;
            case 144: {
                if (record || (instruction & 0x00100800u) != 0) {
                    throw std::runtime_error("Invalid mtcrf instruction at " + Hex(pc) + ".");
                }
                const auto fields = (instruction >> 12) & 0xffu;
                uint32_t mask = 0;
                for (unsigned field = 0; field < 8; ++field) {
                    if ((fields & (0x80u >> field)) != 0) mask |= 0xf0000000u >> (field * 4u);
                }
                output << "    state.cr = (state.cr & ~" << Hex(mask) << "u) | (state.gpr["
                       << rt << "] & " << Hex(mask) << "u);\n";
                emitted = true;
                break;
            }
            case 86:
            case 54:
            case 470:
            case 982:
                if (rt != 0 || record) {
                    throw std::runtime_error("Invalid cache-block instruction at " + Hex(pc) + ".");
                }
                output << "    ppc_flush_cache_block(state, "
                       << (ra == 0 ? "0u" : left) << " + " << right << ");\n";
                emitted = true;
                break;
            case 598:
                if (instruction != 0x7c0004acu) {
                    throw std::runtime_error("Invalid sync instruction at " + Hex(pc) + ".");
                }
                output << "    ppc_sync();\n";
                emitted = true;
                break;
            case 83:
            case 146:
                if (record || ra != 0 || rb != 0) {
                    throw std::runtime_error("Invalid MSR transfer at " + Hex(pc) + ".");
                }
                if (extendedOpcode == 83) {
                    expression = "ppc_read_msr(state)";
                } else {
                    output << "    ppc_write_msr(state, state.gpr[" << rt << "]);\n";
                    emitted = true;
                }
                break;
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
            case 10:
            case 138:
                expression = "ppc_add_carry(state, " + left + ", " + right +
                             (extendedOpcode == 138 ? ", true)" : ")");
                break;
            case 202:
                if (rb != 0) throw std::runtime_error("Invalid addze instruction at " + Hex(pc) + ".");
                expression = "ppc_add_carry(state, " + left + ", 0u, true)";
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
            case 60:
                destination = ra;
                expression = "state.gpr[" + std::to_string(rt) + "] & " +
                    (extendedOpcode == 60 ? "~" : "") + right;
                break;
            case 599:
            case 631:
            case 727:
            case 759: {
                if (record) {
                    throw std::runtime_error("Invalid floating-point indexed instruction at " +
                                             Hex(pc) + ".");
                }
                const std::string base =
                    ra == 0 ? "0u" : "state.gpr[" + std::to_string(ra) + "]";
                EmitFloatingTransfer(output, pc, rt, ra,
                    base + " + state.gpr[" + std::to_string(rb) + "]",
                    extendedOpcode == 599 || extendedOpcode == 631,
                    extendedOpcode == 631 || extendedOpcode == 759);
                emitted = true;
                break;
            }
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
            case 8:
                expression = "ppc_subtract_carry(state, " + left + ", " + right + ")";
                break;
            case 266:
                expression = left + " + " + right;
                break;
            case 235:
                expression = "static_cast<uint32_t>(static_cast<uint64_t>(" +
                             left + ") * " + right + ")";
                break;
            case 11:
                expression = "static_cast<uint32_t>((static_cast<uint64_t>(" +
                             left + ") * " + right + ") >> 32)";
                break;
            case 459:
                expression = "ppc_divide_unsigned32(" + left + ", " + right + ")";
                break;
            case 491:
                expression = "ppc_divide_signed32(" + left + ", " + right + ")";
                break;
            case 444:
                destination = ra;
                expression = "state.gpr[" + std::to_string(rt) + "] | " + right;
                break;
            case 316:
                destination = ra;
                expression = "state.gpr[" + std::to_string(rt) + "] ^ " + right;
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
            case 792:
            case 824:
                destination = ra;
                expression = "ppc_shift_right_arithmetic(state, state.gpr[" +
                    std::to_string(rt) + "], " +
                    (extendedOpcode == 824 ? std::to_string(rb) + "u" : right) + ")";
                break;
            case 339: {
                const uint32_t spr = ra | (rb << 5);
                if (record || (spr != 1 && spr != 8 && spr != 9 && spr != 26 && spr != 27 &&
                               spr != 272 && spr != 1008 && spr != 1017 &&
                               (spr < 912 || spr > 920))) {
                    throw UnsupportedPpcInstruction("Unsupported mfspr instruction " +
                                             Hex(instruction) + " at " +
                                             Hex(pc) + ".");
                }
                expression = spr == 1017 ? "state.l2cr" :
                    spr == 1008 ? "state.hid0" : spr == 920 ? "state.hid2" :
                    spr >= 912 ? "state.gqr[" + std::to_string(spr - 912) + "]" :
                    spr == 26 ? "state.srr0" : spr == 27 ? "state.srr1" : spr == 272 ? "state.sprg0" :
                    spr == 1 ? "state.xer" : spr == 8 ? "state.lr" : "state.ctr";
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
                if (spr == 921) {
                    if (record) throw std::runtime_error("Invalid mtspr WPAR instruction at " + Hex(pc) + ".");
                    output << "    NativeGxSetWriteGatherAddress(state, state.gpr[" << rt << "]);\n";
                    emitted = true;
                    break;
                }
                if (spr == 22) {
                    if (record) throw std::runtime_error("Invalid mtspr DEC instruction at " + Hex(pc) + ".");
                    output << "    ppc_write_decrementer(state, state.gpr[" << rt << "]);\n";
                    emitted = true;
                    break;
                }
                if ((spr >= 528 && spr <= 543) || spr == 26 || spr == 27 || spr == 272) {
                    if (record) throw std::runtime_error("Invalid BAT/SRR transfer at " + Hex(pc) + ".");
                    if (spr == 272) {
                        output << "    state.sprg0 = state.gpr[" << rt << "];\n";
                    } else if (spr == 26 || spr == 27) {
                        output << "    state.srr" << (spr - 26) << " = state.gpr[" << rt << "];\n";
                    } else {
                        output << "    ppc_write_ram_bat(state, " << spr << "u, state.gpr[" << rt << "]);\n";
                    }
                    emitted = true;
                    break;
                }
                if (spr == 952 || spr == 956 || spr == 953 || spr == 954 ||
                    spr == 957 || spr == 958) {
                    if (record) {
                        throw std::runtime_error("Invalid performance-monitor transfer at " +
                                                 Hex(pc) + ".");
                    }
                    const unsigned index = spr == 952 ? 0 : spr == 956 ? 1 :
                        spr == 953 ? 2 : spr == 954 ? 3 : spr == 957 ? 4 : 5;
                    output << "    ppc_reset_performance_monitor(state, " << index
                           << "u, state.gpr[" << rt << "]);\n";
                    emitted = true;
                    break;
                }
                if (record || (spr != 1 && spr != 8 && spr != 9 && spr != 1008 && spr != 1017 &&
                               (spr < 912 || spr > 920))) {
                    throw UnsupportedPpcInstruction("Unsupported mtspr instruction " +
                                             Hex(instruction) + " at " +
                                             Hex(pc) + ".");
                }
                if (spr == 920 || spr == 1008 || spr == 1017) {
                    output << "    ppc_write_" << (spr == 920 ? "hid2" : spr == 1008 ? "hid0" : "l2cr")
                           << "(state, state.gpr[" << rt << "]);\n";
                    emitted = true;
                    break;
                }
                output << "    state."
                       << (spr >= 912 ? "gqr[" + std::to_string(spr - 912) + "]" :
                           spr == 1 ? std::string("xer") : spr == 8 ? std::string("lr") :
                           std::string("ctr"))
                       << " = state.gpr[" << rt << "];\n";
                emitted = true;
                break;
            }
            default:
                throw UnsupportedPpcInstruction("Unsupported PowerPC instruction " +
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
            throw UnsupportedPpcInstruction("Unsupported PowerPC instruction " +
                                     Hex(instruction) + " at " + Hex(pc) +
                                     ".");
        }

        const bool branch = opcode == 16 || opcode == 18 ||
            (opcode == 19 && (Bits(instruction, 1, 10) == 16 ||
                              Bits(instruction, 1, 10) == 528));
        if (!branch && static_cast<uint64_t>(next) < end) {
            output << "    goto " << Label(next) << ";\n";
        } else if (!branch) {
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
