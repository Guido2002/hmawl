#include "native_dsp_task.h"
#include "native_dsp_init.h"

namespace {

void ReceiveLoaderMail(PpcState& state, uint32_t mail) {
    auto& dsp = *state.dspHardware;
    auto next = *dsp.task;
    if (next.mode == NativeDspTaskMode::LoaderCommand) {
        if (next.parameters >= 5) throw std::runtime_error("Native DSP loader parameter sequence is already complete.");
        if ((mail >> 16) != 0x80f3u) throw std::runtime_error("Invalid native DSP loader command prefix.");
        const auto command = static_cast<uint16_t>(mail);
        const uint16_t commands[] = {0xa001, 0xc002, 0xa002, 0xb002, 0xd001};
        const uint16_t expected = commands[next.parameters];
        if (command != expected) throw std::runtime_error("Unsupported or out-of-order native DSP loader command.");
        next.command = command;
        next.mode = NativeDspTaskMode::LoaderValue;
    } else {
        const auto value = mail & 0x7fffffffu;
        switch (next.command) {
        case 0xa001:
            ppc_memory_offset(state, value, 1);
            if (value & 3u) throw std::runtime_error("Native DSP instruction source must be word aligned.");
            next.source = value;
            break;
        case 0xc002:
            if (value >= next.instructionRam.size()) throw std::runtime_error("Native DSP IRAM destination exceeds memory.");
            next.destination = static_cast<uint16_t>(value);
            break;
        case 0xa002:
            if (!value || (value & 3u) || value > (next.instructionRam.size() - next.destination) * 2) {
                throw std::runtime_error("Invalid native DSP instruction DMA length.");
            }
            ppc_memory_offset(state, next.source, value);
            next.bytes = static_cast<uint16_t>(value);
            break;
        case 0xb002:
            if (value != 0) throw std::runtime_error("Native DSP loader DRAM upload is not implemented.");
            next.dataBytes = 0;
            break;
        case 0xd001: {
            if (value < next.destination || value >= next.destination + next.bytes / 2) {
                throw std::runtime_error("Native DSP task entry is outside the uploaded program.");
            }
            if (!state.validateDspTask || !state.executeDspTask) {
                throw std::runtime_error("Native DSP task requires a build-time compiled microcode binding.");
            }
            state.validateDspTask(state, next.source, next.destination, next.bytes, static_cast<uint16_t>(value));
            for (unsigned offset = 0; offset < next.bytes; offset += 2) {
                next.instructionRam[next.destination + offset / 2] = ppc_read_u16(state, next.source + offset);
            }
            next.entry = static_cast<uint16_t>(value);
            ++next.instructionUploads;
            next.uploadedBytes += next.bytes;
            next.mode = NativeDspTaskMode::Running;
            break;
        }
        default: throw std::runtime_error("Native DSP loader has no pending parameter.");
        }
        ++next.parameters;
        if (next.mode != NativeDspTaskMode::Running) next.mode = NativeDspTaskMode::LoaderCommand;
    }
    ++next.cpuMailSent;
    ++next.cpuMailConsumed;
    *dsp.task = next;
    dsp.toDspMailbox = mail & 0x7fffffffu;
    if (next.mode == NativeDspTaskMode::Running) dsp.instructionPc = next.entry;
}

} // namespace

void NativeDspStartTaskLoader(PpcState& state) {
    if (!state.dspHardware || !state.dspHardware->task || !state.dspControl ||
        (*state.dspControl & 0x805u) != 0x800u ||
        state.dspHardware->task->mode != NativeDspTaskMode::Inactive) {
        throw std::runtime_error("Unsupported native DSP task-loader handoff.");
    }
    auto& dsp = *state.dspHardware;
    if (dsp.fromDspMailbox & 0x80000000u) {
        throw std::runtime_error("Native DSP loader cannot overwrite unread output mail.");
    }
    // Explicit BIOS-free loader service, not execution or replacement of a DSP task.
    NativeDspWriteOutput(dsp, 0xfffc, 0x8071);
    NativeDspWriteOutput(dsp, 0xfffd, 0xfeed);
    dsp.task->mode = NativeDspTaskMode::LoaderCommand;
    dsp.task->command = dsp.task->parameters = 0;
    dsp.task->source = 0;
    dsp.task->destination = dsp.task->bytes = dsp.task->dataBytes = dsp.task->entry = 0;
    dsp.task->registers.fill(0);
    std::fill(dsp.task->registers.begin() + 8, dsp.task->registers.begin() + 12, 0xffff);
    dsp.task->dmaControl = dsp.task->dmaAddress = 0;
    dsp.task->dmaMainAddress = 0;
    dsp.task->returnAddresses.clear();
    dsp.task->loops.clear();
    ++dsp.task->loaderStarts;
}

void NativeDspWriteCpuMail(PpcState& state, uint32_t address, uint16_t value) {
    auto& dsp = *state.dspHardware;
    if (!dsp.task || dsp.programHalted || (dsp.toDspMailbox & 0x80000000u) ||
        (dsp.task->mode != NativeDspTaskMode::Running &&
         dsp.task->mode != NativeDspTaskMode::LoaderCommand &&
         dsp.task->mode != NativeDspTaskMode::LoaderValue)) {
        throw std::runtime_error("Native DSP CPU mailbox requires an active native task or loader.");
    }
    if (address == 0xcc005000u) {
        dsp.toDspMailbox = (dsp.toDspMailbox & 0xffffu) | (uint32_t(value & 0x7fffu) << 16);
    } else if (address == 0xcc005002u) {
        const auto mail = (dsp.toDspMailbox & 0xffff0000u) | value | 0x80000000u;
        if (dsp.task->mode == NativeDspTaskMode::Running) {
            dsp.toDspMailbox = mail;
            ++dsp.task->cpuMailSent;
        } else {
            if (dsp.fromDspMailbox & 0x80000000u) {
                throw std::runtime_error("Native DSP loader greeting has not been consumed.");
            }
            ReceiveLoaderMail(state, mail);
        }
    } else {
        throw std::runtime_error("Invalid native DSP CPU mailbox register.");
    }
}

uint16_t NativeDspTaskRead(PpcState& state, uint16_t address) {
    auto& dsp = *state.dspHardware;
    if (address < dsp.dataRam.size()) return dsp.dataRam[address];
    switch (address) {
    case 0xffc9: return dsp.task->dmaControl;
    case 0xffcb: return 0;
    case 0xffcd: return dsp.task->dmaAddress;
    case 0xffce: return static_cast<uint16_t>(dsp.task->dmaMainAddress >> 16);
    case 0xffcf: return static_cast<uint16_t>(dsp.task->dmaMainAddress);
    case 0xfffc: return static_cast<uint16_t>(dsp.fromDspMailbox >> 16);
    case 0xfffd: return static_cast<uint16_t>(dsp.fromDspMailbox);
    case 0xfffe: return static_cast<uint16_t>(dsp.toDspMailbox >> 16);
    case 0xffff: {
        const auto value = static_cast<uint16_t>(dsp.toDspMailbox);
        if (dsp.toDspMailbox & 0x80000000u) {
            dsp.toDspMailbox &= 0x7fffffffu;
            ++dsp.task->cpuMailConsumed;
        }
        return value;
    }
    default: throw std::runtime_error("Unsupported native DSP task memory read at " + std::to_string(address) + ".");
    }
}

void NativeDspTaskWrite(PpcState& state, uint16_t address, uint16_t value) {
    auto& dsp = *state.dspHardware;
    if (address < dsp.dataRam.size()) {
        dsp.dataRam[address] = value;
    } else if (address == 0xfffc || address == 0xfffd) {
        NativeDspWriteOutput(dsp, address, value);
    } else if (address == 0xfffb && value == 1) {
        *state.dspControl |= 0x80u;
        ++dsp.task->interrupts;
    } else if (address == 0xffc9) {
        if (value > 1) throw std::runtime_error("Native DSP supports only synchronous data RAM DMA.");
        dsp.task->dmaControl = value;
    } else if (address == 0xffcd) {
        dsp.task->dmaAddress = value;
    } else if (address == 0xffce) {
        dsp.task->dmaMainAddress = (dsp.task->dmaMainAddress & 0xffffu) | (uint32_t(value) << 16);
    } else if (address == 0xffcf) {
        dsp.task->dmaMainAddress = (dsp.task->dmaMainAddress & 0xffff0000u) | value;
    } else if (address == 0xffcb) {
        auto& task = *dsp.task;
        if (!value || (value & 1u) || (task.dmaMainAddress & 1u) ||
            task.dmaAddress >= dsp.dataRam.size() ||
            value / 2u > dsp.dataRam.size() - task.dmaAddress) {
            throw std::runtime_error("Invalid native DSP data DMA alignment or range.");
        }
        const auto offset = ppc_memory_offset(state, task.dmaMainAddress, value);
        if (task.dataTransfers == UINT32_MAX || task.transferredBytes > UINT64_MAX - value) {
            throw std::runtime_error("Native DSP data DMA accounting overflow.");
        }
        for (unsigned i = 0; i < value / 2u; ++i) {
            auto& word = dsp.dataRam[task.dmaAddress + i];
            if (task.dmaControl == 0) {
                word = uint16_t((uint16_t(state.memory[offset + 2 * i]) << 8) |
                                state.memory[offset + 2 * i + 1]);
            } else {
                state.memory[offset + 2 * i] = static_cast<uint8_t>(word >> 8);
                state.memory[offset + 2 * i + 1] = static_cast<uint8_t>(word);
            }
        }
        ++task.dataTransfers;
        task.transferredBytes += value;
    } else {
        throw std::runtime_error("Unsupported native DSP task memory write at " + std::to_string(address) + ".");
    }
}

namespace {

int64_t Signed40(uint64_t bits) {
    bits &= UINT64_C(0xffffffffff);
    return bits & UINT64_C(0x8000000000) ? int64_t(bits) - INT64_C(0x10000000000) : int64_t(bits);
}

} // namespace

int64_t NativeDspAccumulator(const PpcState& state, unsigned acc) {
    if (acc > 1) throw std::runtime_error("Invalid native DSP accumulator.");
    const auto& r = state.dspHardware->task->registers;
    return Signed40((uint64_t(r[16 + acc] & 0xffu) << 32) |
                    (uint64_t(r[30 + acc]) << 16) | r[28 + acc]);
}

void NativeDspSetAccumulator(PpcState& state, unsigned acc, int64_t value) {
    if (acc > 1) throw std::runtime_error("Invalid native DSP accumulator.");
    auto& r = state.dspHardware->task->registers;
    const auto bits = uint64_t(value);
    r[16 + acc] = static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(bits >> 32)));
    r[30 + acc] = static_cast<uint16_t>(bits >> 16);
    r[28 + acc] = static_cast<uint16_t>(bits);
}

void NativeDspArithmeticFlags(PpcState& state, int64_t left, int64_t right, bool subtract) {
    const auto result = Signed40(uint64_t(subtract ? left - right : left + right));
    const bool carry = subtract ? uint64_t(left) >= uint64_t(result) : uint64_t(left) > uint64_t(result);
    const auto operand = subtract ? -right : right;
    const bool overflow = ((left ^ result) & (operand ^ result)) < 0;
    uint16_t flags = carry ? 1 : 0;
    if (overflow) flags |= 0x82;
    if (!result) flags |= 4;
    if (result < 0) flags |= 8;
    if (result < INT32_MIN || result > INT32_MAX) flags |= 0x10;
    const auto top = uint64_t(result) & 0xc0000000u;
    if (top == 0 || top == 0xc0000000u) flags |= 0x20;
    auto& sr = state.dspHardware->task->registers[19];
    sr = (sr & ~0x3fu) | flags;
}

bool NativeDspCondition(const PpcState& state, unsigned condition) {
    const auto sr = state.dspHardware->task->registers[19];
    const bool less = bool(sr & 2) != bool(sr & 8), zero = (sr & 4) != 0;
    switch (condition) {
    case 0: return !less;
    case 1: return less;
    case 2: return !less && !zero;
    case 3: return less || zero;
    case 4: return !zero;
    case 5: return zero;
    case 6: return !(sr & 1);
    case 7: return (sr & 1) != 0;
    case 8: return !(sr & 0x10);
    case 9: return (sr & 0x10) != 0;
    case 10: return (sr & 0x30) && !zero;
    case 11: return !(sr & 0x30) || zero;
    case 12: return !(sr & 0x40);
    case 13: return (sr & 0x40) != 0;
    case 14: return (sr & 2) != 0;
    case 15: return true;
    default: throw std::runtime_error("Invalid native DSP branch condition.");
    }
}

void NativeDspLogicFlags(PpcState& state, unsigned acc) {
    NativeDspArithmeticFlags(state, NativeDspAccumulator(state, acc), 0, false);
    auto& r = state.dspHardware->task->registers;
    r[19] &= ~0xcu;
    if (!r[30 + acc]) r[19] |= 4;
    if (r[30 + acc] & 0x8000u) r[19] |= 8;
}

void NativeDspCall(PpcState& state, uint16_t resume) {
    auto& task = *state.dspHardware->task;
    auto& stack = task.returnAddresses;
    if (stack.size() + task.loops.size() >= 4) throw std::runtime_error("Native DSP call stack exceeds the bounded profile.");
    stack.push_back(resume);
}

uint16_t NativeDspReturn(PpcState& state) {
    auto& stack = state.dspHardware->task->returnAddresses;
    if (stack.empty()) throw std::runtime_error("Native DSP return stack is empty.");
    const auto resume = stack.back();
    stack.pop_back();
    return resume;
}

void NativeDspStartLoop(PpcState& state, uint16_t start, uint16_t end, uint16_t count, uint16_t after) {
    auto& task = *state.dspHardware->task;
    if (start > end || after <= end) throw std::runtime_error("Invalid native DSP loop range.");
    if (count && task.loops.size() + task.returnAddresses.size() >= 4) {
        throw std::runtime_error("Native DSP loop stack exceeds the bounded profile.");
    }
    if (count) task.loops.push_back({start, end, count});
    state.dspHardware->instructionPc = count ? start : after;
}

uint16_t NativeDspNext(PpcState& state, uint16_t pc, uint16_t next) {
    auto& loops = state.dspHardware->task->loops;
    if (!loops.empty() && loops.back().end == pc) {
        if (--loops.back().remaining) return loops.back().start;
        loops.pop_back();
    }
    return next;
}

void NativeDspTaskWriteRegister(PpcState& state, unsigned reg, uint16_t value) {
    auto& r = state.dspHardware->task->registers;
    if (reg >= r.size() || (reg >= 12 && reg <= 15)) {
        throw std::runtime_error("Unsupported native DSP stack/register write.");
    }
    if (reg == 18) value &= 0xff;
    if (reg == 19) value &= ~0x100u;
    if (reg == 16 || reg == 17) {
        value = static_cast<uint16_t>(static_cast<int16_t>(static_cast<int8_t>(value)));
    }
    r[reg] = value;
    if ((reg == 30 || reg == 31) && (r[19] & 0x4000u)) {
        r[reg - 14] = value & 0x8000u ? 0xffffu : 0;
        r[reg - 2] = 0;
    }
}

uint16_t NativeDspTaskReadRegister(const PpcState& state, unsigned reg) {
    const auto& r = state.dspHardware->task->registers;
    if (reg >= r.size() || (reg >= 12 && reg <= 15)) {
        throw std::runtime_error("Unsupported native DSP stack/register read.");
    }
    if ((reg == 30 || reg == 31) && (r[19] & 0x4000u)) {
        const int64_t value = int64_t(static_cast<int16_t>(r[reg - 14])) * INT64_C(0x100000000) +
                              (uint32_t(r[reg]) << 16) + r[reg - 2];
        if (value > INT32_MAX) return 0x7fff;
        if (value < INT32_MIN) return 0x8000;
    }
    return r[reg];
}

void NativeDspAdvanceTask(PpcState& state) {
    if (!state.dspHardware || !state.dspHardware->task ||
        state.dspHardware->task->mode != NativeDspTaskMode::Running) return;
    if (!state.dspControl || (*state.dspControl & 4u)) return;
    if (!state.executeDspTask) throw std::runtime_error("Native DSP task has no compiled execution binding.");
    state.executeDspTask(state);
}
