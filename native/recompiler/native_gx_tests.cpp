#include "ppc_recompiler.h"
#include "ppc_state.h"

#include <iostream>
#include <stdexcept>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void Reject(Function function, const char* reason) {
    try {
        function();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(reason) != std::string::npos,
                "Graphics operation failed for an unexpected reason.");
        return;
    }
    throw std::runtime_error("Unsupported graphics operation silently succeeded.");
}

PpcState State() {
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(0x2000, 0xa5);
    state.graphics.emplace();
    state.externalInterrupt.emplace();
    state.piInterruptMask = 0xf0;
    ppc_write_u32(state, 0xcc00300cu, 0x1000);
    ppc_write_u32(state, 0xcc003010u, 0x107c);
    ppc_write_u32(state, 0xcc003014u, 0x1000);
    for (unsigned index = 0; index < 8; ++index) {
        const uint32_t value = index == 0 || index == 5 || index == 6 ? 0x1000 :
                               index == 1 ? 0x107c : index == 2 ? 0x60 : 0;
        ppc_write_u16(state, 0xcc000020u + index * 4, static_cast<uint16_t>(value));
        ppc_write_u16(state, 0xcc000022u + index * 4, 0);
    }
    NativeGxSetWriteGatherAddress(state, 0x0c008000);
    ppc_write_hid2(state, 0x40000000);
    ppc_write_u16(state, 0xcc000002u, 0x11);
    return state;
}

void Send(PpcState& state, std::vector<uint8_t> bytes) {
    while ((bytes.size() + state.graphics->gathered.size()) % 32 != 0) bytes.push_back(0);
    for (const auto byte : bytes) ppc_write_u8(state, 0xcc008000u, byte);
}

void TestGatherAndCommands() {
    auto state = State();
    auto& gx = *state.graphics;
    Require(ppc_read_u32(state, 0xcc003010u) == 0x1060 &&
                ppc_read_u16(state, 0xcc000024u) == 0x1060,
            "PI/CP end register did not mask unused low address bits.");
    ppc_write_u8(state, 0xcc008000u, 0x08);
    ppc_write_u8(state, 0xcc008000u, 0x50);
    ppc_write_u16(state, 0xcc008000u, 0x1234);
    ppc_write_u32(state, 0xcc008000u, 0x56780000);
    Require(gx.bursts == 0 && gx.cp[0x50] == 0 && state.memory[0x1000] == 0xa5,
            "FIFO decoded or wrote RAM before a complete gather burst.");
    Send(state, {});
    Require(gx.bursts == 1 && gx.cp[0x50] == 0x12345678 && gx.commands.empty() &&
                gx.fifo[4] == 0 && gx.fifo[5] == 0x1020 && gx.fifo[6] == 0x1020 &&
                gx.cpuWrite == 0x1020 && gx.decodedCommands == 27,
            "Mixed-width big-endian gather/CP decoding/pointer advancement failed.");
    Require(state.memory[0x1000] == 8 && state.memory[0x1001] == 0x50 &&
                state.memory[0x1002] == 0x12 && state.memory[0x1005] == 0x78 &&
                state.memory[0xfff] == 0xa5 && state.memory[0x1080] == 0xa5,
            "Actual FIFO burst RAM or exclusive guards differ.");

    std::vector<uint8_t> commands(30, 0);
    commands.insert(commands.end(), {0x61, 0x41, 0x12, 0x34, 0x56,
        0x61, 0xfe, 0x00, 0x00, 0xff, 0x61, 0x41, 0xab, 0xcd, 0xef,
        0x10, 0x00, 0x01, 0x10, 0x00, 0x3f, 0x80, 0, 0, 0x40, 0, 0, 0, 0x48});
    Send(state, commands);
    Require(gx.bp[0x41] == 0x1234ef && gx.bpMask == 0xffffff &&
                gx.xf[0x1000] == 0x3f800000 && gx.xf[0x1001] == 0x40000000 &&
                gx.vertexCacheInvalidations == 1 && gx.commands.empty(),
            "Split-burst BP masking, XF load or vertex-cache invalidation failed.");
    Send(state, std::vector<uint8_t>(64));
    Require(gx.bursts == 5 && gx.cpuWrite == 0x1020 && gx.fifo[5] == 0x1020 &&
                gx.fifo[6] == 0x1020 && state.memory[0x1080] == 0xa5,
            "FIFO burst wraparound escaped its configured ring.");
    // XF header plus two paired stores must stream to the same hardware FIFO port.
    ppc_write_u8(state, 0xcc008000u, 0x10);
    ppc_write_u32(state, 0xcc008000u, 0x00031004);
    ppc_store_float_pair(state, 0xcc008000u,
                         {ppc_widen_single_bits(0x3f800000), ppc_widen_single_bits(0)}, 0, false);
    ppc_store_float_pair(state, 0xcc008000u,
                         {ppc_widen_single_bits(0), ppc_widen_single_bits(0x3f800000)}, 0, false);
    Send(state, {});
    Require(gx.xf[0x1004] == 0x3f800000 && gx.xf[0x1005] == 0 &&
                gx.xf[0x1006] == 0 && gx.xf[0x1007] == 0x3f800000,
            "Paired matrix stores did not stream both lanes to actual XF command state.");
}

void TestInterruptsAndReader() {
    auto state = State();
    auto& gx = *state.graphics;
    const uint16_t pokeValues[] = {0x17, 0xffff, 0x1a5, 0x7cd, 6};
    for (unsigned index = 0; index < 5; ++index) {
        const auto address = 0xcc001000u + index * 2;
        Require(ppc_read_u16(state, address) == 0, "PE poke configuration did not start reset.");
        ppc_write_u16(state, address, pokeValues[index]);
        Require(ppc_read_u16(state, address) == pokeValues[index] && gx.poke[index] == pokeValues[index],
                "PE poke depth/blend/alpha configuration was not retained.");
    }
    for (const auto address : {0xcc001000u, 0xcc001004u, 0xcc001008u}) {
        const auto before = gx.poke;
        Reject([&] { ppc_write_u16(state, address, 0x8000); }, "poke configuration");
        Require(gx.poke == before, "Invalid PE poke configuration modified state.");
    }
    Reject([&] { ppc_read_u8(state, 0xcc001002u); }, "access width");
    Reject([&] { ppc_write_u32(state, 0xcc001002u, 0); }, "access width");
    ppc_write_u32(state, 0xcc003004u, 0xef0);
    ppc_write_u16(state, 0xcc00100au, 15);
    Send(state, {0x61, 0x48, 0x00, 0x12, 0x34, 0x61, 0x45, 0, 0, 2});
    Require(ppc_read_u16(state, 0xcc00100eu) == 0x1234 &&
                ppc_read_u16(state, 0xcc00100au) == 3 && gx.pePending == 12 &&
                ppc_read_u32(state, 0xcc003000u) == 0x600 &&
                ppc_external_interrupt_pending(state),
            "Decoded token/finish did not assert their actual PE interrupt sources.");
    ppc_write_u16(state, 0xcc00100au, 7);
    Require(NativeGxInterruptCause(gx) == 0x400 && gx.peControl == 3 && gx.pePending == 8,
            "PE W1C acknowledgement cleared the wrong pending interrupt.");
    ppc_write_u16(state, 0xcc00100au, 11);
    Require(!ppc_external_interrupt_pending(state) && gx.peControl == 3 && gx.pePending == 0,
            "PE finish W1C did not retain interrupt enables.");
    ppc_write_u16(state, 0xcc000002u, 0x14);
    gx.fifo[2] = 0;
    Send(state, {0x08, 0x60, 1, 2, 3, 4});
    Require(gx.fifo[4] == 32 && gx.cp[0x60] == 0 &&
                NativeGxInterruptCause(gx) == 0x800,
            "Disabled reader consumed a command or missed its high watermark interrupt.");
    ppc_write_u16(state, 0xcc000004u, 1);
    Require(NativeGxInterruptCause(gx) == 0, "CP clear did not acknowledge latched overflow.");
    const auto bursts = gx.bursts, commands = gx.decodedCommands;
    Require(gx.performanceBursts != 0 && gx.performanceCommands != 0,
            "Native performance interval did not account for actual FIFO work.");
    ppc_write_u16(state, 0xcc000006u, 5);
    ppc_write_u16(state, 0xcc000004u, 4);
    Require(gx.performanceResets == 1 && gx.performanceBursts == 0 && gx.performanceCommands == 0 &&
                gx.bursts == bursts && gx.decodedCommands == commands &&
                ppc_read_u16(state, 0xcc000006u) == 5 && ppc_read_u16(state, 0xcc00000eu) == 0x1234,
            "CP metric reset altered lifetime work, selector or actual token register.");
    gx.fifo[3] = 32;
    ppc_write_u16(state, 0xcc000002u, 0x19);
    Require(gx.cp[0x60] == 0x01020304 && gx.fifo[4] == 0 &&
                NativeGxInterruptCause(gx) == 0x800,
            "Reader enable failed to drain actual RAM or latch underflow.");
    ppc_write_u16(state, 0xcc000004u, 2);
    Require(ppc_read_u16(state, 0xcc000000u) == 12 &&
                !ppc_external_interrupt_pending(state),
            "CP did not derive idle status from actual consumption.");
    ppc_write_u16(state, 0xcc000002u, 0);
    ppc_write_u16(state, 0xcc00003cu, static_cast<uint16_t>(gx.fifo[6]));
    ppc_write_u16(state, 0xcc00003eu, 0);
    ppc_write_u16(state, 0xcc000002u, 0x33);
    const auto oldRead = gx.fifo[6];
    Send(state, {0x08, 0x50, 0x12, 0x34, 0x56, 0x78});
    Require(gx.fifo[4] == 32 && gx.fifo[6] == oldRead && gx.cp[0x50] == 0 &&
                (ppc_read_u16(state, 0xcc000000u) & 0x10u) != 0 &&
                NativeGxInterruptCause(gx) == 0x800,
            "CP read past a configured breakpoint or failed to assert its interrupt.");
    ppc_write_u16(state, 0xcc000002u, 0x11);
    Require(gx.fifo[4] == 0 && gx.cp[0x50] == 0x12345678 &&
                NativeGxInterruptCause(gx) == 0,
            "Disabling the breakpoint did not resume actual buffered consumption.");
}

void TestBurstBoundaries() {
    auto state = State();
    ppc_write_u16(state, 0xcc000002u, 0x10);
    Send(state, std::vector<uint8_t>(128));
    Require(state.graphics->fifo[4] == 128 && state.graphics->cpuWrite == 0x1000 &&
                state.graphics->bursts == 4 && state.graphics->decodedCommands == 0,
            "Disabled reader failed to retain all four bursts in a full FIFO.");
    for (unsigned byte = 0; byte < 31; ++byte) ppc_write_u8(state, 0xcc008000u, 0);
    const auto memory = state.memory;
    const auto gathered = state.graphics->gathered;
    Reject([&] { ppc_write_u8(state, 0xcc008000u, 0); }, "mismatched or full");
    Require(state.memory == memory && state.graphics->gathered == gathered &&
                state.graphics->fifo[4] == 128 && state.graphics->bursts == 4,
            "Full FIFO failure overwrote unread bytes or changed the gather buffer.");
    ppc_write_u16(state, 0xcc000002u, 0x11);
    ppc_write_u8(state, 0xcc008000u, 0);
    Require(state.graphics->bursts == 5 && state.graphics->fifo[4] == 0 &&
                state.graphics->decodedCommands == 160,
            "FIFO did not recover after the reader freed real space.");
    auto invalid = State();
    ppc_write_u16(invalid, 0xcc000002u, 0);
    ppc_write_u16(invalid, 0xcc000038u, 0x1080);
    ppc_write_u16(invalid, 0xcc000030u, 32);
    const auto registers = invalid.graphics->fifo;
    Reject([&] { ppc_write_u16(invalid, 0xcc000002u, 1); }, "ring bounds");
    Require(invalid.graphics->control == 0 && invalid.graphics->fifo == registers &&
                invalid.graphics->decodedCommands == 0,
            "Invalid reader activation partially enabled or consumed a malformed ring.");
}

void TestExplicitBoundaries() {
    auto state = State();
    const auto control = state.graphics->control;
    Reject([&] { ppc_write_u16(state, 0xcc000002u, 0x80); }, "CP control");
    Require(state.graphics->control == control, "Invalid CP control modified state.");
    Reject([&] { ppc_write_u32(state, 0xcc000002u, 0); }, "access width");
    Reject([&] { ppc_read_u8(state, 0xcc00100au); }, "access width");
    Reject([&] { ppc_write_u16(state, 0xcc000020u, 0); }, "reader disabled");
    Reject([&] { NativeGxSetWriteGatherAddress(state, 0); }, "WPAR");
    Reject([&] { Send(state, {0x80, 0, 1}); }, "vertex/renderer");
    Require(state.graphics->decodedCommands == 0,
            "Unsupported draw was counted as successfully consumed.");
    auto copy = State();
    Reject([&] { Send(copy, {0x61, 0x52, 0, 0, 0}); }, "renderer/copy");
    Require(copy.graphics->bp[0x52] == 0 && copy.graphics->decodedCommands == 0,
            "Unimplemented EFB copy reported success or changed BP state.");
    auto xf = State();
    Reject([&] { Send(xf, {0x10, 0, 1, 0x10, 0x57, 0, 0, 0, 1, 0, 0, 0, 2}); }, "XF load");
    Require(xf.graphics->xf.back() == 0, "Out-of-bounds XF load partially committed.");
    auto tlut = State();
    Reject([&] { Send(tlut, {0x61, 0x65, 0, 0, 0}); }, "texture-memory");
    auto opcode = State();
    Reject([&] { Send(opcode, {0x40, 0, 0, 0, 0}); }, "FIFO opcode");
    auto fragmented = State();
    ppc_write_hid2(fragmented, 0);
    const auto noGatherMemory = fragmented.memory;
    Reject([&] { ppc_write_u8(fragmented, 0xcc008000u, 0); }, "write gathering");
    Require(fragmented.graphics->gathered.empty() && fragmented.memory == noGatherMemory,
            "Disabled write gathering still modified memory or buffered a command.");
    auto missing = State();
    ppc_write_hid2(missing, 0);
    missing.graphics.reset();
    Reject([&] { ppc_write_u32(missing, 0xcc003004u, 0x8f0); }, "mapped hardware");
    Require(missing.piInterruptMask == 0xf0, "Missing graphics profile changed PI mask.");
    Reject([&] { ppc_write_hid2(missing, 0x40000000); }, "native GX");
    Require(RecompileFunction(0x80004000, {0x7c79e3a6}).find("NativeGxSetWriteGatherAddress") !=
                std::string::npos,
            "Original WPAR instruction did not compile to checked write-gather setup.");
    Reject([&] { RecompileFunction(0x80004000, {0x7c79e3a7}); }, "WPAR");
}

} // namespace

int main() {
    try {
        TestGatherAndCommands();
        TestInterruptsAndReader();
        TestBurstBoundaries();
        TestExplicitBoundaries();
    } catch (const std::exception& error) {
        std::cerr << "Native GX test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Native GX FIFO RAM/gather, CP/BP/XF commands, ring wrap, interrupt sources "
                 "and explicit draw/copy boundaries passed. No renderer ran.\n";
}
