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
    } else {
        throw std::runtime_error("Unsupported native DSP task memory write at " + std::to_string(address) + ".");
    }
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
