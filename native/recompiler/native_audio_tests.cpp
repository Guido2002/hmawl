#include "ppc_state.h"

#include <iostream>
#include <stdexcept>

namespace {

void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

template <typename F> void Reject(F operation) {
    bool rejected = false;
    try { operation(); }
    catch (const std::exception&) { rejected = true; }
    Require(rejected, "Unsupported audio operation succeeded.");
}

PpcState AudioState(uint64_t& ticks) {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(256, 0xa5);
    state.audio.emplace();
    state.aiControl = 0;
    state.dspControl = 4;
    state.dspHardware.emplace();
    state.dspHardware->aram.resize(256, 0x5a);
    state.readTimeBase = [&ticks] { return ticks; };
    return state;
}

void TestAiClockAndInterrupts() {
    uint64_t ticks = 0;
    for (uint32_t rate : {0u, 2u}) {
        const uint64_t denominator = rate ? 6744 : 10116;
        auto state = AudioState(ticks);
        ticks = 0;
        ppc_write_u32(state, 0xcc006c00u, 1 | rate);
        auto split = state;
        NativeAiAdvance(state, denominator * 100 + 11);
        for (uint64_t tick = 1; tick <= denominator * 100 + 11; ++tick) {
            NativeAiAdvance(split, tick);
        }
        Require(state.audio->samples == 800 && state.audio->phase == 88 &&
                    state.audio->advancedSamples == 800 &&
                    state.audio->samples == split.audio->samples &&
                    state.audio->phase == split.audio->phase &&
                    state.aiControl == split.aiControl,
                "AI rational clock depends on advance granularity or uses the wrong rate.");
        ticks = denominator * 100 + 11;
        ppc_write_u32(state, 0xcc006c00u, rate);
        ticks += denominator * 100;
        NativeAiAdvance(state, ticks);
        Require(state.audio->samples == 800, "Paused AI accumulated samples.");
        ppc_write_u32(state, 0xcc006c00u, 1 | rate);
        NativeAiAdvance(state, ticks + denominator);
        Require(state.audio->samples == 808, "Resumed AI included paused time.");
        const auto before = *state.audio;
        Reject([&] { NativeAiAdvance(state, ticks); });
        Require(state.audio->samples == before.samples && state.audio->phase == before.phase &&
                    state.audio->lastTicks == before.lastTicks, "Backwards AI time partially committed.");
    }
    ticks = 0;
    auto state = AudioState(ticks);
    ppc_write_u32(state, 0xcc006c0cu, 5);
    ppc_write_u32(state, 0xcc006c00u, 3);
    ticks = 6744;
    NativeAiAdvance(state, ticks);
    Require(state.audio->samples == 8 && (*state.aiControl & 8) && NativeAiInterruptCause(state) == 0,
            "Masked AI threshold did not latch status independently of the IRQ mask.");
    ppc_write_u32(state, 0xcc006c00u, 7);
    Require(NativeAiInterruptCause(state) == 0x20 && ppc_pi_interrupt_cause(state) == 0x20,
            "Unmasking AI lost the pending PI source.");
    ppc_write_u32(state, 0xcc006c00u, 15);
    Require(*state.aiControl == 7 && NativeAiInterruptCause(state) == 0, "AI W1C did not acknowledge.");
    *state.dspControl = 0x64;
    state.piInterruptMask = 0x40;
    Require(ppc_pi_interrupt_cause(state) == 0x40 && !ppc_external_interrupt_pending(state),
            "DSP interrupt was confused with AI or MI, or routed without DSP delivery support.");
    Reject([&] { ppc_write_msr(state, 0x8000); });
    *state.dspControl = 4;
    ppc_write_u32(state, 0xcc006c08u, 0xfffffffeu);
    ppc_write_u32(state, 0xcc006c0cu, 1);
    ticks += 6744;
    NativeAiAdvance(state, ticks);
    Require(state.audio->samples == 6 && (*state.aiControl & 8), "AI threshold failed across counter wrap.");
    ppc_write_u32(state, 0xcc006c00u, 0x3f);
    Require(state.audio->samples == 0 && state.audio->phase == 0 && *state.aiControl == 0x17 &&
                state.audio->resets == 1, "AI reset/W1C did not self-clear correctly.");
    ppc_write_u32(state, 0xcc006c0cu, 5);
    ticks += 6744;
    NativeAiAdvance(state, ticks);
    Require(state.audio->samples == 8 && (*state.aiControl & 8) == 0, "AI valid/inhibit bit failed.");
    ppc_write_u32(state, 0xcc006c00u, 15);
    ppc_write_u32(state, 0xcc006c0cu, 8);
    ticks += 6744;
    NativeAiAdvance(state, ticks);
    Require((*state.aiControl & 8) == 0, "AI equal threshold re-fired before a full wrap.");
    auto overflow = state;
    overflow.audio->advancedSamples = UINT64_MAX;
    Reject([&] { NativeAiAdvance(overflow, ticks + 6744); });
    Require(overflow.audio->samples == state.audio->samples &&
                overflow.audio->lastTicks == ticks && overflow.aiControl == state.aiControl,
            "AI accounting overflow partially committed.");
    auto large = AudioState(ticks);
    *large.aiControl = 3;
    NativeAiAdvance(large, 0);
    NativeAiAdvance(large, UINT64_MAX);
    Require(large.audio->advancedSamples == (UINT64_MAX / 6744) * 8 + (UINT64_MAX % 6744) * 8 / 6744,
            "Large elapsed AI time overflowed its rational calculation.");
    ticks = 0;
    auto changed = AudioState(ticks);
    ppc_write_u32(changed, 0xcc006c00u, 3);
    ticks = 6743;
    ppc_write_u32(changed, 0xcc006c00u, 1);
    Require(changed.audio->samples == 7 && changed.audio->phase == 0,
            "AI rate change lost old-rate samples or carried an incompatible fractional phase.");
    ticks += 10116;
    NativeAiAdvance(changed, ticks);
    Require(changed.audio->samples == 15, "AI rate change did not select the 32 kHz divisor.");
}

void TestAiRegistersAndDmaGuards() {
    uint64_t ticks = 0;
    auto state = AudioState(ticks);
    ppc_write_u32(state, 0xcc006c04u, 0x12ab);
    Require(ppc_read_u32(state, 0xcc006c04u) == 0x12ab, "AI stereo volume channels were not preserved.");
    for (const auto address : {0xcc006c00u, 0xcc006c04u, 0xcc006c08u, 0xcc006c0cu}) {
        Reject([&] { ppc_read_u16(state, address); });
        Reject([&] { ppc_write_u16(state, address, 0); });
        Reject([&] { ppc_write_u8(state, address, 0); });
        Reject([&] { NativeAiRead(state, address + 1, 4); });
    }
    Reject([&] { ppc_write_u32(state, 0xcc006c00u, 0x80); });
    Reject([&] { ppc_write_u32(state, 0xcc006c04u, 0x10000); });
    Require(state.audio->volume == 0x12ab && state.audio->writes == 1 && state.aiControl == 0,
            "Invalid AI write modified its registers or accounting.");
    ppc_write_u16(state, 0xcc005030u, 0xffff);
    ppc_write_u16(state, 0xcc005032u, 0xffff);
    ppc_write_u16(state, 0xcc005036u, 0x123);
    Require(ppc_read_u16(state, 0xcc005030u) == 0x3ff &&
                ppc_read_u16(state, 0xcc005032u) == 0xffe0 &&
                ppc_read_u16(state, 0xcc005036u) == 0x123, "Disabled PCM DMA lost its register configuration.");
    Reject([&] { ppc_write_u16(state, 0xcc005036u, 0x8123); });
    Require(state.dspHardware->audioDmaBlocks == 0x123, "Unimplemented PCM enable fabricated playback.");
    state.readTimeBase = {};
    Reject([&] { ppc_write_u32(state, 0xcc006c04u, 0); });
    Require(state.audio->volume == 0x12ab, "Missing AI time source partially committed a write.");
}

void TestDspTaskLoader() {
    uint64_t ticks = 0;
    auto state = AudioState(ticks);
    auto& dsp = *state.dspHardware;
    dsp.task.emplace();
    dsp.programHalted = false;
    *state.dspControl = 0x800;
    state.executeDspInitialization = NativeDspStartTaskLoader;
    NativeDspStartTaskLoader(state);
    Require(dsp.fromDspMailbox == 0x8071feed && dsp.sentMail == 1 &&
                dsp.completedPrograms == 0 && dsp.executedInstructions == 0 &&
                dsp.task->loaderStarts == 1, "Native loader greeting was confused with executed task code.");
    const auto send = [&](uint32_t mail) {
        NativeDspWriteCpuMail(state, 0xcc005000, static_cast<uint16_t>(mail >> 16));
        NativeDspWriteCpuMail(state, 0xcc005002, static_cast<uint16_t>(mail));
    };
    Reject([&] { send(0x80f3a001); });
    Require(dsp.task->cpuMailConsumed == 0 && dsp.task->parameters == 0,
            "Loader accepted mail before its greeting was consumed.");
    ppc_read_u16(state, 0xcc005006);
    Reject([&] { send(0x1234a001); });
    Reject([&] { send(0x80f3a002); });
    Require(dsp.task->mode == NativeDspTaskMode::LoaderCommand && dsp.task->cpuMailConsumed == 0,
            "Rejected native loader command advanced its protocol.");
    send(0x80f3a001);
    Reject([&] { send(0x80000001); });
    send(0x80000020);
    send(0x80f3c002);
    Reject([&] { send(4096); });
    send(0);
    send(0x80f3a002);
    for (uint32_t invalid : {0u, 1u, 8196u, 256u}) Reject([&] { send(invalid); });
    send(8);
    send(0x80f3b002);
    Reject([&] { send(4); });
    send(0);
    send(0x80f3d001);
    Reject([&] { send(4); });
    Reject([&] { send(0); });
    Require(dsp.task->instructionUploads == 0 && dsp.task->cpuMailConsumed == 9 &&
                dsp.instructionPc == 0, "Missing native task binding fabricated a successful upload.");
    state.executeDspTask = [](PpcState&) { throw std::runtime_error("Test task was not compiled."); };
    state.validateDspTask = [](const PpcState&, uint32_t, uint16_t, uint16_t, uint16_t) {
        throw std::runtime_error("Test task image mismatch.");
    };
    Reject([&] { send(0); });
    Require(dsp.task->instructionUploads == 0 && dsp.task->cpuMailConsumed == 9 &&
                dsp.task->instructionRam[0] == 0 && dsp.completedPrograms == 0,
            "Failed task validation partially committed an instruction upload.");
    state.validateDspTask = [](const PpcState& guest, uint32_t source, uint16_t destination,
                              uint16_t bytes, uint16_t entry) {
        Require(source == 32 && destination == 0 && bytes == 8 && entry == 0 &&
                    ppc_read_u32(guest, source) == 0xa5a5a5a5,
                "Loader validation received different DMA/entry parameters.");
    };
    send(0);
    Require(dsp.task->instructionUploads == 1 && dsp.task->uploadedBytes == 8 &&
                dsp.task->cpuMailSent == 10 && dsp.task->cpuMailConsumed == 10 &&
                dsp.task->instructionRam[0] == 0xa5a5 && dsp.task->instructionRam[3] == 0xa5a5 &&
                dsp.task->instructionRam[4] == 0 && dsp.task->mode == NativeDspTaskMode::Running &&
                dsp.executedInstructions == 0 && dsp.sentMail == 1 && dsp.completedPrograms == 0,
            "Loader did not copy real RAM bytes or fabricated task initialization output.");
    Reject([&] { NativeDspAdvanceTask(state); });
    Require(dsp.sentMail == 1 && dsp.completedPrograms == 0,
            "Missing compiled task execution fabricated mail or completion.");
    ppc_write_u16(state, 0xcc00500a, 0x805);
    Require(dsp.task->mode == NativeDspTaskMode::Inactive && dsp.programHalted && dsp.resetCount == 1 &&
                dsp.toDspMailbox == 0 && dsp.fromDspMailbox == 0,
            "DSP reset did not discard the live loader/task protocol and pending mail.");
    ppc_write_u16(state, 0xcc00500a, 0x800);
    Require(dsp.task->mode == NativeDspTaskMode::LoaderCommand && dsp.task->parameters == 0 &&
                dsp.task->loaderStarts == 2 && dsp.task->instructionUploads == 1 &&
                dsp.fromDspMailbox == 0x8071feed && !dsp.programHalted &&
                dsp.task->registers == std::array<uint16_t, 32>{},
            "Restarted native loader retained parameters/registers or erased historical DMA evidence.");
}

void TestAramRegistersAndTransfers() {
    uint64_t ticks = 0;
    auto state = AudioState(ticks);
    auto& dsp = *state.dspHardware;
    Reject([&] { ppc_read_u16(state, 0xcc005016u); });
    dsp.aramControllerReady = true;
    Require(ppc_read_u16(state, 0xcc005016u) == 1, "Initialized ARAM is not ready.");
    for (uint16_t configuration : {3, 0x23, 0x43, 0x63}) {
        ppc_write_u16(state, 0xcc005012u, configuration);
        Require(ppc_read_u16(state, 0xcc005012u) == configuration, "ARAM probe configuration was lost.");
    }
    ppc_write_u16(state, 0xcc00501au, 176);
    Reject([&] { ppc_write_u16(state, 0xcc00501au, 0x800); });
    Reject([&] { ppc_write_u16(state, 0xcc005012u, 0); });
    Reject([&] { ppc_write_u16(state, 0xcc005016u, 1); });
    Require(dsp.aramRefresh == 176 && dsp.aramConfiguration == 0x63, "Invalid ARAM write partially committed.");
    ppc_write_u16(state, 0xcc005020u, 0xfc00);
    ppc_write_u16(state, 0xcc005022u, 0x3f);
    ppc_write_u16(state, 0xcc005024u, 0);
    ppc_write_u16(state, 0xcc005026u, 0x5f);
    Require(ppc_read_u32(state, 0xcc005020u) == 32 && ppc_read_u32(state, 0xcc005024u) == 64,
            "Split ARAM addresses did not mask reserved/unaligned bits.");
    ppc_write_u16(state, 0xcc005028u, 0);
    Require(dsp.dmaTransfers == 0, "ARAM count high word started DMA.");
    ppc_write_u16(state, 0xcc00502au, 63);
    Require(dsp.dmaTransfers == 1 && dsp.dmaCount == 0 && dsp.dmaMainAddress == 64 &&
                dsp.dmaAramAddress == 96 && (*state.dspControl & 0x20) &&
                dsp.aram[63] == 0x5a && dsp.aram[64] == 0xa5 && dsp.aram[95] == 0xa5 &&
                dsp.aram[96] == 0x5a, "Split ARAM write DMA changed the wrong bytes or completion state.");
    ppc_write_u16(state, 0xcc00500au, 0x24);
    Require(*state.dspControl == 4, "ARAM interrupt W1C failed.");
    ppc_write_u32(state, 0xcc005020u, 128);
    ppc_write_u32(state, 0xcc005024u, 64);
    std::fill(state.memory.begin() + 128, state.memory.begin() + 160, 0x11);
    ppc_write_u16(state, 0xcc005028u, 0x8000);
    ppc_write_u16(state, 0xcc00502au, 32);
    Require(dsp.dmaCount == 0x80000000 && dsp.dmaMainAddress == 160 &&
                dsp.dmaAramAddress == 96 && state.memory[127] == 0xa5 &&
                std::all_of(state.memory.begin() + 128, state.memory.begin() + 160,
                            [](uint8_t byte) { return byte == 0xa5; }) && state.memory[160] == 0xa5,
            "Split ARAM read DMA did not complete.");
    const auto memory = state.memory;
    const auto aram = dsp.aram;
    ppc_write_u32(state, 0xcc005020u, 224);
    Reject([&] { ppc_write_u32(state, 0xcc005028u, 64); });
    Require(state.memory == memory && dsp.aram == aram && dsp.dmaTransfers == 2 &&
                dsp.dmaMainAddress == 224 && dsp.dmaCount == 0x80000000,
            "Out-of-RAM ARAM DMA partially committed.");
    dsp.aram.resize(0x1000000, 0x5a);
    ppc_write_u32(state, 0xcc005020u, 0);
    ppc_write_u32(state, 0xcc005024u, 0x1000000);
    Reject([&] { ppc_write_u32(state, 0xcc005028u, 0x80000020); });
    dsp.disconnectedAramExpansion = true;
    ppc_write_u32(state, 0xcc005028u, 0x80000020);
    ppc_write_u32(state, 0xcc005028u, 32);
    Require(state.memory == memory && dsp.absentAramReads == 1 && dsp.absentAramWrites == 1 &&
                dsp.aram.front() == 0x5a && dsp.aram.back() == 0x5a,
            "Disconnected expansion fabricated incoming data or backing storage.");
    ppc_write_u32(state, 0xcc005024u, 0xffffe0);
    Reject([&] { ppc_write_u32(state, 0xcc005028u, 64); });
    Require(dsp.absentAramReads == 1 && dsp.absentAramWrites == 1, "Straddling ARAM DMA silently completed.");
    ppc_write_u32(state, 0xcc005024u, 0x3ffffe0);
    ppc_write_u32(state, 0xcc005028u, 32);
    Require(dsp.dmaAramAddress == 0 && dsp.absentAramWrites == 2, "ARAM DMA address did not wrap to register width.");
}

} // namespace

int main() {
    try {
        TestAiClockAndInterrupts();
        TestAiRegistersAndDmaGuards();
        TestAramRegistersAndTransfers();
        TestDspTaskLoader();
        std::cout << "Native AI/ARAM/task-loader tests passed; no PCM playback or synthetic task execution claimed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Native audio test failure: " << error.what() << '\n';
        return 1;
    }
}
