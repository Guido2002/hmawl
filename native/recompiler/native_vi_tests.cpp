#include "ppc_state.h"

#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void Reject(Function function, const char* message) {
    bool rejected = false;
    try {
        function();
    } catch (const std::exception& error) {
        rejected = std::string(error.what()).find(message) != std::string::npos;
    }
    Require(rejected, "Expected explicit VI dependency/invalid-input failure.");
}

PpcState State() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.video.emplace();
    state.piInterruptMask = 0xf0;
    return state;
}

void TestRegisters() {
    auto state = State();
    for (unsigned offset : {0u, 4u, 6u, 8u, 0x1cu, 0x28u, 0x48u, 0x66u, 0x70u}) {
        Require(ppc_read_u16(state, 0xcc002000u + offset) == 0, "VI did not start cold.");
        ppc_write_u16(state, 0xcc002000u + offset, 0x1234);
        Require(ppc_read_u16(state, 0xcc002000u + offset) == 0x1234, "VI configuration readback differs.");
    }
    ppc_write_u32(state, 0xcc00204cu, 0x1ae771f0);
    Require(ppc_read_u16(state, 0xcc00204cu) == 0x1ae7 &&
                ppc_read_u16(state, 0xcc00204eu) == 0x71f0 &&
                ppc_read_u32(state, 0xcc00204cu) == 0x1ae771f0,
            "VI big-endian split coefficient register differs.");
    for (auto address : {0xcc002002u, 0xcc00206cu}) {
        const auto before = state.video->registers;
        Reject([&] { ppc_write_u16(state, address, 0xffff); }, "Native VI");
        Require(state.video->registers == before, "Unsupported VI mode mutated registers.");
    }
    const auto before = state.video->registers;
    Reject([&] { ppc_write_u32(state, 0xcc00206cu, 1); }, "register write");
    Require(state.video->registers == before, "Rejected wide VI write partially committed.");
    Require(ppc_read_u8(state, 0xcc00204cu) == 0x1a &&
                ppc_read_u8(state, 0xcc00204du) == 0xe7,
            "VI byte reads did not select the big-endian halfword lane.");
    Reject([&] { ppc_write_u8(state, 0xcc002002u, 1); }, "access width");
    Reject([&] { ppc_read_u32(state, 0xcc002002u); }, "alignment");
    Reject([&] { ppc_write_u16(state, 0xcc00202cu, 1); }, "register write");
    Reject([&] { ppc_read_u16(state, 0xcc002068u); }, "register read");
    Require(ppc_read_u16(state, 0xcc00206eu) == 0, "Disconnected digital-cable pins changed.");
    ppc_write_u32(state, 0xcc002030u, 0x110701ae);
    const auto configured = state.video->registers;
    ppc_write_u16(state, 0xcc002002u, 2);
    Require(state.video->resets == 1 && state.video->registers[1] == 0 &&
                state.video->registers[0x30 / 2] == 0 && state.video->registers[0x32 / 2] == 0 &&
                state.video->registers[0x4c / 2] == configured[0x4c / 2] &&
                state.video->registers[3] == configured[3] && !state.video->clockStarted,
            "VI reset did not clear interrupt/scan state while preserving programmed timing/coefficients.");
    auto absent = state;
    absent.video.reset();
    Reject([&] { ppc_read_u16(absent, 0xcc002002u); }, "not mapped");
    Reject([&] { ppc_write_u32(absent, 0xcc003004u, 0x1f0); }, "interrupt mask");
}

void TestBeamAndInterrupts() {
    auto state = State();
    auto& vi = *state.video;
    ppc_write_u16(state, 0xcc002006u, 429);
    ppc_write_u32(state, 0xcc002030u, 0x110701ae); // SDK NTSC second-field compare.
    ppc_write_u32(state, 0xcc002034u, 0x10010001); // First beam pixel of next frame.
    ppc_write_u16(state, 0xcc002002u, 1);
    Reject([&] { ppc_read_u16(state, 0xcc00202cu); }, "time base");
    constexpr uint64_t start = 1000000, field = 429 * 525 * 3, frame = field * 2;
    constexpr uint64_t target = ((263 - 1) * 858 + 430 - 1) * 3;
    NativeViAdvance(vi, start);
    const auto timing = vi.registers;
    Reject([&] { ppc_write_u16(state, 0xcc002006u, 430); }, "live scan-clock");
    Require(vi.registers == timing, "Rejected live VI clock change mutated timing.");
    Require(ppc_read_u16(state, 0xcc00202cu) == 1 && ppc_read_u16(state, 0xcc00202eu) == 1 &&
                NativeViInterruptCause(vi) == 0,
            "VI startup did not begin at the first beam pixel without fabricating an interrupt.");
    NativeViAdvance(vi, start + target - 1);
    Require(NativeViInterruptCause(vi) == 0 && vi.fields == 0,
            "VI asserted display interrupt before the programmed beam threshold.");
    NativeViAdvance(vi, start + target);
    Require(ppc_read_u16(state, 0xcc00202cu) == 263 && ppc_read_u16(state, 0xcc00202eu) == 430 &&
                NativeViInterruptCause(vi) == 0x100 && !ppc_external_interrupt_pending(state) &&
                vi.fields == 1,
            "VI beam threshold, field count or PI masking differs.");
    ppc_write_u32(state, 0xcc003004u, 0x1f0);
    Require(ppc_external_interrupt_pending(state), "PI did not expose a real enabled VI source.");
    ppc_write_u16(state, 0xcc002030u, 0x9107);
    Require(NativeViInterruptCause(vi) == 0x100, "VI write-one incorrectly acknowledged a pending source.");
    ppc_write_u16(state, 0xcc002030u, 0x1107);
    Require(NativeViInterruptCause(vi) == 0, "VI write-zero did not acknowledge its own source.");
    NativeViAdvance(vi, start + frame - 1);
    Require(ppc_read_u16(state, 0xcc00202cu) == 525 && ppc_read_u16(state, 0xcc00202eu) == 858 &&
                NativeViInterruptCause(vi) == 0,
            "VI interlaced frame wrapped early.");
    NativeViAdvance(vi, start + frame);
    Require(vi.fields == 2 && NativeViInterruptCause(vi) == 0x100 &&
                ppc_read_u16(state, 0xcc00202cu) == 1 && ppc_read_u16(state, 0xcc00202eu) == 1,
            "VI frame wrap did not latch the independent first-pixel comparator.");
    ppc_write_u16(state, 0xcc002034u, 0x1001);
    Require(NativeViInterruptCause(vi) == 0, "VI acknowledgement did not clear DI1.");
    const auto elapsed = vi.elapsedTicks, last = vi.lastTicks;
    Reject([&] { NativeViAdvance(vi, last - 1); }, "backwards");
    Require(vi.elapsedTicks == elapsed && vi.lastTicks == last, "Backwards VI clock mutated timing.");
    NativeViAdvance(vi, start + frame * 4);
    Require(vi.fields == 8 && NativeViInterruptCause(vi) == 0x100,
            "Large monotonic VI time advance lost field counts or latched comparator events.");
    ppc_write_u16(state, 0xcc002030u, 0x0107);
    ppc_write_u16(state, 0xcc002034u, 0x0001);
    NativeViAdvance(vi, start + frame * 5);
    Require(NativeViInterruptCause(vi) == 0 && (ppc_read_u16(state, 0xcc002030u) & 0x8000u),
            "Masked VI comparator did not latch independently of PI delivery.");
    ppc_write_u16(state, 0xcc002030u, 0x9107);
    Require(NativeViInterruptCause(vi) == 0x100,
            "Unmasking an already latched VI comparator lost the pending source.");
    ppc_write_u16(state, 0xcc002030u, 0x1107);
    ppc_write_u32(state, 0xcc002038u, 0x10000001);
    const auto invalid = vi;
    Reject([&] { NativeViAdvance(vi, start + frame * 6); }, "invalid NTSC beam");
    Require(vi.registers == invalid.registers && vi.elapsedTicks == invalid.elapsedTicks &&
                vi.lastTicks == invalid.lastTicks && vi.fields == invalid.fields,
            "Invalid later VI comparator partially committed an earlier comparator or time.");
    ppc_write_u32(state, 0xcc002038u, 0);
    ppc_write_u16(state, 0xcc002002u, 0);
    NativeViAdvance(vi, start + frame * 6);
    Require(vi.fields == 10, "Disabled VI advanced its scan clock.");
}

} // namespace

int main() {
    try {
        TestRegisters();
        TestBeamAndInterrupts();
    } catch (const std::exception& error) {
        std::cerr << "Native VI test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Native VI cold/reset/configuration, NTSC beam timing, exact display interrupt thresholds, "
                 "PI masking/acknowledgement and explicit unsupported modes passed. No pixels rendered.\n";
}
