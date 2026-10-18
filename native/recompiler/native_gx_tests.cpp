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
        if (std::string(error.what()).find(reason) == std::string::npos) {
            throw std::runtime_error(std::string("Graphics operation expected '") + reason +
                                     "' but failed with: " + error.what());
        }
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
    Reject([&] { Send(state, {0x80, 0, 1}); }, "direct XY");
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
    Reject([&] { Send(tlut, {0x61, 0x65, 0, 0, 0}); }, "TLUT-memory");
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

PpcState RenderState() {
    auto state = State();
    auto& gx = *state.graphics;
    gx.efb.emplace();
    std::fill(gx.efb->color.begin(), gx.efb->color.end(), 0x000000ff);
    std::fill(gx.efb->depth.begin(), gx.efb->depth.end(), 0xffffff);
    std::fill(gx.efb->defined.begin(), gx.efb->defined.end(), 3);
    gx.cp[0x30] = 60 << 6;
    gx.cp[0x50] = 0x2200;
    gx.cp[0x60] = 1;
    gx.cp[0x70] = 0x01216006;
    gx.bp[0] = 0x4011;
    gx.bp[0x21] = (639 << 12) | 527;
    gx.bp[0x40] = 0x17;
    gx.bp[0x41] = 0x4b9;
    gx.bp[0x28] = 0x40;
    gx.bp[0x30] = gx.bp[0x31] = 3;
    gx.bp[0x80] = 0x190;
    gx.bp[0x88] = (5 << 20) | (3 << 10) | 3;
    gx.bp[0x94] = 0x800 >> 5;
    gx.bp[0xc0] = 0x8f8af;
    gx.bp[0xc1] = 0x8f2f0;
    gx.bp[0xf0] = 1;
    gx.bp[0xf3] = 0x3f0000;
    gx.xf[0] = gx.xf[5] = gx.xf[10] = 0x3f800000;
    gx.xf[0x1009] = 1;
    gx.xf[0x1008] = 0x11;
    gx.xf[0x1018] = gx.cp[0x30];
    gx.xf[0x100e] = gx.xf[0x1010] = 0x401;
    gx.xf[0x101a] = gx.xf[0x101b] = gx.xf[0x1020] = gx.xf[0x1022] = 0x3f800000;
    gx.xf[0x101d] = gx.xf[0x101e] = 0x43ab0000; // GX viewport's 342-pixel bias.
    gx.xf[0x1026] = gx.xf[0x103f] = 1;
    gx.xf[0x1040] = 0x280;
    gx.xf[0x1050] = 0x3d;
    for (unsigned i = 0; i < 16; ++i) ppc_write_u16(state, 0x800 + i * 2, 0xfc00);
    return state;
}

std::vector<uint8_t> Quad(uint32_t color = 0xffffffff) {
    std::vector<uint8_t> bytes{0x80, 0, 4};
    const auto word = [&](uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) bytes.push_back(uint8_t(value >> shift));
    };
    for (unsigned i = 0; i < 4; ++i) {
        bytes.push_back(0); bytes.push_back(i == 1 || i == 2 ? 4 : 0);
        bytes.push_back(0); bytes.push_back(i >= 2 ? 4 : 0);
        word(color);
        word(i == 1 || i == 2 ? 0x3f800000 : 0);
        word(i >= 2 ? 0x3f800000 : 0);
    }
    return bytes;
}

void TestQuadRenderer() {
    auto state = RenderState();
    const auto bytes = Quad();
    for (unsigned i = 0; i < 64; ++i) ppc_write_u8(state, 0xcc008000, bytes[i]);
    Require(state.graphics->draws == 0 && state.graphics->commands.size() == 64 &&
                state.graphics->efb->color[0] == 0x000000ff,
            "Incomplete quad was rasterized or consumed across a gather boundary.");
    Send(state, {bytes.begin() + 64, bytes.end()});
    Require(state.graphics->draws == 1 && state.graphics->rasterizedPixels == 16 &&
                state.graphics->commands.empty(),
            "Complete split quad did not rasterize exactly sixteen pixels once.");
    for (unsigned y = 0; y < 5; ++y) {
        for (unsigned x = 0; x < 5; ++x)
            Require(state.graphics->efb->color[y * 640 + x] ==
                        (x < 4 && y < 4 ? 0xff0000ff : 0x000000ff),
                    "Quad coverage, RGB555 expansion, shared edge or viewport differs.");
    }
    auto blend = RenderState();
    Send(blend, Quad(0xffffff80));
    Require(blend.graphics->rasterizedPixels == 16 && blend.graphics->efb->color[641] == 0x800000ff &&
                blend.graphics->efb->color[642] == 0x800000ff,
            "Alpha blending double-shaded the quad diagonal or used the wrong factors.");
    auto transparent = RenderState();
    for (unsigned i = 0; i < 16; ++i) ppc_write_u16(transparent, 0x800 + i * 2, 0x0f00);
    transparent.graphics->bp[0xf3] = 0x240000;
    Send(transparent, Quad());
    Require(transparent.graphics->rasterizedPixels == 0 &&
                transparent.graphics->efb->color[0] == 0x000000ff,
            "RGB5A3 transparent alpha test did not suppress the fragment.");
    auto scissor = RenderState();
    scissor.graphics->bp[0x20] = (1 << 12) | 2;
    scissor.graphics->bp[0x21] = (2 << 12) | 3;
    Send(scissor, Quad());
    Require(scissor.graphics->rasterizedPixels == 4 &&
                scissor.graphics->efb->color[1281] == 0xff0000ff &&
                scissor.graphics->efb->color[641] == 0x000000ff,
            "GX scissor X/Y fields or inclusive limits differ.");
    auto depth = RenderState();
    depth.graphics->xf[0x101f] = 0x3f800000;
    std::fill(depth.graphics->efb->depth.begin(), depth.graphics->efb->depth.end(), 0);
    Send(depth, Quad());
    Require(depth.graphics->rasterizedPixels == 0, "Z24 less-equal comparison ignored destination depth.");
    for (const uint32_t clipZ : {0x3f000000u, 0xbfc00000u}) {
        auto outside = RenderState();
        outside.graphics->xf[0x1025] = clipZ;
        const auto before = *outside.graphics->efb;
        Send(outside, Quad());
        Require(outside.graphics->draws == 1 && outside.graphics->rasterizedPixels == 0 &&
                    outside.graphics->efb->color == before.color &&
                    outside.graphics->efb->depth == before.depth &&
                    outside.graphics->efb->defined == before.defined &&
                    outside.graphics->commands.empty(),
                "Fully near/far-clipped quad changed pixels or failed to consume its primitive.");
    }
    for (const bool near : {false, true}) {
        auto clipped = RenderState();
        clipped.graphics->xf[8] = 0x3f000000; // Position Z varies from zero to two across X.
        clipped.graphics->xf[0x1024] = 0x3f800000;
        clipped.graphics->xf[0x1025] = near ? 0xbf800000 : 0xc0000000;
        clipped.graphics->xf[0x101c] = clipped.graphics->xf[0x101f] = 0x43800000;
        for (unsigned y = 0; y < 4; ++y)
            for (unsigned x = 0; x < 4; ++x)
                ppc_write_u16(clipped, 0x800 + y * 8 + x * 2, x < 2 ? 0xfc00 : 0x801f);
        auto reference = clipped;
        reference.graphics->xf[0x1024] = reference.graphics->xf[0x1025] = 0;
        auto gradient = Quad(0xffffff80);
        for (unsigned i = 0; i < 4; ++i) {
            const bool right = i == 1 || i == 2;
            gradient[7 + i * 16] = right ? 0x20 : 0xff;
            gradient[8 + i * 16] = right ? 0xff : 0x20;
            gradient[9 + i * 16] = right ? 0x40 : 0x80;
        }
        Send(reference, gradient);
        Send(clipped, gradient);
        Require(clipped.graphics->draws == 1 && clipped.graphics->rasterizedPixels == 8,
                "Near/far plane crossing did not cover exactly half the quad.");
        for (unsigned y = 0; y < 4; ++y) {
            for (unsigned x = 0; x < 4; ++x) {
                const auto index = y * NativeEfb::width + x;
                const bool visible = near ? x < 2 : x >= 2;
                Require(clipped.graphics->efb->color[index] ==
                            (visible ? reference.graphics->efb->color[index] : 0x000000ff) &&
                            clipped.graphics->efb->depth[index] ==
                            (visible ? (x % 2 == 0 ? 64u : 192u) : 0xffffffu),
                        "Depth clipping changed interpolated texture/blend values, seams or Z24.");
            }
        }
    }
    for (const uint32_t clipZ : {0xbf800000u, 0u}) {
        auto boundary = RenderState();
        boundary.graphics->xf[0x1025] = clipZ;
        boundary.graphics->xf[0x101c] = boundary.graphics->xf[0x101f] = 0x43800000;
        Send(boundary, Quad());
        Require(boundary.graphics->rasterizedPixels == 16 &&
                    boundary.graphics->efb->depth[641] == (clipZ == 0 ? 256u : 0u),
                "Quad exactly on the near/far clip plane was rejected or mapped to the wrong depth.");
    }
    auto bothPlanes = RenderState();
    bothPlanes.graphics->xf[8] = bothPlanes.graphics->xf[0x1024] = 0x3f800000;
    bothPlanes.graphics->xf[0x1025] = 0xc0000000;
    bothPlanes.graphics->xf[0x101c] = bothPlanes.graphics->xf[0x101f] = 0x43800000;
    Send(bothPlanes, Quad());
    Require(bothPlanes.graphics->rasterizedPixels == 4 &&
                bothPlanes.graphics->efb->color[641] == 0xff0000ff &&
                bothPlanes.graphics->efb->depth[641] == 128 &&
                bothPlanes.graphics->efb->color[640] == 0x000000ff &&
                bothPlanes.graphics->efb->color[642] == 0x000000ff,
            "Clipping a triangle against both depth planes lost its four-pixel strip.");
    auto tiled = RenderState();
    tiled.graphics->bp[0x88] = (5 << 20) | (3 << 10) | 7;
    tiled.graphics->bp[0x30] = 7;
    tiled.graphics->bp[0x80] = 0x100;
    for (unsigned i = 0; i < 16; ++i) ppc_write_u16(tiled, 0x820 + i * 2, 0x83e0);
    Send(tiled, Quad());
    Require(tiled.graphics->efb->color[0] == 0xff0000ff &&
                tiled.graphics->efb->color[3] == 0x00ff00ff,
            "RGB5A3 tile addressing or nearest sampling crossed the wrong tile.");
    auto a3 = RenderState();
    for (unsigned i = 0; i < 16; ++i) ppc_write_u16(a3, 0x800 + i * 2, 0x7f00);
    Send(a3, Quad());
    Require(a3.graphics->efb->color[0] == 0xff0000ff, "A3RGB4 expansion differs from opaque RGB555.");
    auto filtered = RenderState();
    filtered.graphics->bp[0x88] = (5 << 20) | (3 << 10) | 1;
    filtered.graphics->bp[0x30] = 1;
    for (unsigned y = 0; y < 4; ++y) {
        ppc_write_u16(filtered, 0x800 + y * 8, 0xfc00);
        ppc_write_u16(filtered, 0x802 + y * 8, 0x801f);
    }
    Send(filtered, Quad());
    Require(filtered.graphics->efb->color[0] == 0xff0000ff &&
                filtered.graphics->efb->color[1] == 0xbf0040ff &&
                filtered.graphics->efb->color[2] == 0x4000bfff &&
                filtered.graphics->efb->color[3] == 0x0000ffff,
            "Bilinear half-texel positions or edge clamp differ.");
    for (const uint32_t mode : {0x100u, 0x190u, 0x1f90u, 0x10100u, 0x1ff90u}) {
        for (const uint32_t lod : {0x800u, 0xff00u, 0xffffu, 0x80u}) {
            auto reference = RenderState();
            reference.graphics->bp[0x80] = mode & 0x19f;
            for (unsigned y = 0; y < 4; ++y)
                for (unsigned x = 0; x < 4; ++x)
                    ppc_write_u16(reference, 0x800 + y * 8 + x * 2, x < 2 ? 0xfc00 : 0x801f);
            auto limited = reference;
            limited.graphics->bp[0x80] = mode;
            limited.graphics->bp[0x84] = lod;
            Send(reference, Quad());
            Send(limited, Quad());
            Require(limited.graphics->draws == 1 && limited.graphics->rasterizedPixels == 16 &&
                        limited.graphics->efb->color == reference.graphics->efb->color &&
                        limited.graphics->efb->depth == reference.graphics->efb->depth &&
                        limited.graphics->efb->defined == reference.graphics->efb->defined,
                    "Non-mipmapped LOD limits/bias changed nearest/bilinear base-level pixels.");
        }
    }
    auto laterTexture = RenderState();
    laterTexture.memory.resize(0x20000, 0xa5);
    laterTexture.graphics->bp[0x88] = 0x51f966;
    laterTexture.graphics->bp[0x30] = 0x166;
    laterTexture.graphics->bp[0x31] = 0x7e;
    laterTexture.graphics->bp[0x84] = 0x800;
    laterTexture.graphics->bp[0x80] = 0x1f90;
    laterTexture.graphics->bp[0x94] = 0x4000 >> 5;
    constexpr unsigned laterTextureBytes = 90 * 32 * 32;
    for (unsigned offset = 0; offset < laterTextureBytes; offset += 2)
        ppc_write_u16(laterTexture, 0x4000 + offset, 0x83e0);
    Send(laterTexture, Quad());
    Require(laterTexture.graphics->rasterizedPixels == 16 &&
                laterTexture.graphics->efb->color[641] == 0x00ff00ff &&
                laterTexture.memory[0x3fff] == 0xa5 &&
                laterTexture.memory[0x4000 + laterTextureBytes] == 0xa5,
            "Observed 359x127 RGB5A3 texture with max LOD 0x800 did not render its base level.");
    auto wrap = RenderState();
    wrap.graphics->bp[0x80] = 0x101;
    // A separate texture matrix translates S by one full repeat.
    wrap.graphics->cp[0x30] = wrap.graphics->xf[0x1018] = 30 << 6;
    wrap.graphics->xf[120] = wrap.graphics->xf[125] = 0x3f800000;
    wrap.graphics->xf[123] = 0x3f800000;
    Send(wrap, Quad());
    Require(wrap.graphics->efb->color[0] == 0xff0000ff &&
                wrap.graphics->rasterizedPixels == 16, "Texture-matrix translation/repeat wrap differs.");
    auto fade = RenderState();
    auto& fg = *fade.graphics;
    fg.cp[0x50] = 0x200; fg.cp[0x60] = 0; fg.cp[0x70] = 7;
    fg.bp[0] = 0x4010; fg.bp[0x28] = 0;
    fg.bp[0xc0] = 0x8fffa; fg.bp[0xc1] = 0x8ffd0;
    fg.xf[0x100e] = fg.xf[0x1010] = 0x400;
    fg.xf[0x1008] = 0;
    fg.xf[0x100c] = 0x00000080; fg.xf[0x103f] = 0;
    std::fill(fg.efb->color.begin(), fg.efb->color.end(), 0xff0000ff);
    std::vector<uint8_t> fadeBytes{0x80, 0, 4};
    for (unsigned i = 0; i < 4; ++i)
        fadeBytes.insert(fadeBytes.end(), {0, uint8_t(i == 1 || i == 2 ? 4 : 0),
                                          0, uint8_t(i >= 2 ? 4 : 0), 0, 0});
    Send(fade, fadeBytes);
    Require(fg.draws == 1 && fg.rasterizedPixels == 16 && fg.efb->color[641] == 0x7f0000ff,
            "Direct XYZ/material-color fade did not blend over the textured EFB.");
    for (unsigned profile = 0; profile < 4; ++profile) {
        for (const bool textured : {false, true}) {
            auto reference = textured ? RenderState() : fade;
            auto inactiveFog = reference;
            const uint32_t fogWords[] = {
                profile == 0 ? 0u : profile == 1 ? 0x7f800u : 0xffffffu,
                profile == 0 ? 0x40000eu : profile == 1 ? 0x7ae165u : 0xffffffu,
                profile == 0 ? 1u : profile == 1 ? 0x1eu : 0xffffffu,
                profile < 2 ? 0u : profile == 2 ? 0x100000u : 0x1fffffu,
                profile < 2 ? 0u : 0x123456u
            };
            std::vector<uint8_t> fogCommands;
            for (unsigned index = 0; index < 5; ++index) {
                const auto word = ((0xeeu + index) << 24) | fogWords[index];
                fogCommands.push_back(0x61);
                for (int shift = 24; shift >= 0; shift -= 8)
                    fogCommands.push_back(uint8_t(word >> shift));
            }
            Send(inactiveFog, fogCommands);
            Send(reference, textured ? Quad(0xffffff80) : fadeBytes);
            Send(inactiveFog, textured ? Quad(0xffffff80) : fadeBytes);
            Require(inactiveFog.graphics->draws == reference.graphics->draws &&
                        inactiveFog.graphics->rasterizedPixels == reference.graphics->rasterizedPixels &&
                        inactiveFog.graphics->efb->color == reference.graphics->efb->color &&
                        inactiveFog.graphics->efb->depth == reference.graphics->efb->depth &&
                        inactiveFog.graphics->efb->defined == reference.graphics->efb->defined &&
                        inactiveFog.graphics->commands.empty(),
                    "Disabled fog coefficients/color/projection changed textured or fade pixels.");
            for (unsigned index = 0; index < 5; ++index)
                Require(inactiveFog.graphics->bp[0xee + index] == fogWords[index],
                        "Disabled fog command words were discarded instead of retained.");
        }
    }
    for (unsigned fogType = 1; fogType < 8; ++fogType) {
        auto enabledFog = RenderState();
        enabledFog.graphics->bp[0xf1] = fogType << 21;
        const auto before = *enabledFog.graphics->efb;
        Reject([&] { Send(enabledFog, Quad()); }, "lighting/projection/PE/fog/indirect");
        Require(enabledFog.graphics->draws == 0 && enabledFog.graphics->rasterizedPixels == 0 &&
                    enabledFog.graphics->efb->color == before.color &&
                    enabledFog.graphics->efb->depth == before.depth &&
                    enabledFog.graphics->efb->defined == before.defined &&
                    enabledFog.graphics->commands[0] == 0x80 &&
                    enabledFog.graphics->decodedCommands == 0,
                "Unsupported enabled fog modified EFB or consumed its primitive.");
        enabledFog.graphics->bp[0xf1] = 0x1fffff;
        ppc_write_u16(enabledFog, 0xcc000002, 0x11);
        Require(enabledFog.graphics->draws == 1 && enabledFog.graphics->rasterizedPixels == 16 &&
                    enabledFog.graphics->commands.empty(),
                "Disabling fog did not retry the retained primitive exactly once.");
    }
    auto culled = RenderState();
    culled.graphics->bp[0] = 0x8011;
    Send(culled, Quad());
    Require(culled.graphics->draws == 1 && culled.graphics->rasterizedPixels == 0,
            "GX front/back culling orientation differs.");
    for (unsigned failure = 0; failure < 13; ++failure) {
        auto invalid = RenderState();
        if (failure == 0) invalid.graphics->bp[0xc0] = 0;
        if (failure == 1) invalid.graphics->bp[0x88] = 0;
        if (failure == 2) invalid.graphics->xf[0x1024] = 0x7fc00000;
        if (failure == 4) invalid.graphics->bp[0x84] = 0x10000;
        if (failure >= 5 && failure < 9) {
            constexpr unsigned mipFilters[] = {1, 2, 5, 6};
            invalid.graphics->bp[0x80] = 0x110 | (mipFilters[failure - 5] << 5);
        }
        if (failure == 9) invalid.graphics->bp[0x80] |= 1 << 17;
        if (failure == 10) invalid.graphics->bp[0x80] |= 1 << 19;
        if (failure == 11) invalid.graphics->bp[0x80] &= ~16u;
        if (failure == 12) invalid.graphics->xf[0x1005] = 1;
        auto command = Quad();
        if (failure == 3) command[2] = 3;
        const auto before = *invalid.graphics->efb;
        Reject([&] { Send(invalid, command); }, failure == 12 ? "lighting/projection" : failure == 0 ? "TEV" :
            failure == 2 ? "non-finite" : failure == 3 ? "groups of four" : "RGB5A3");
        Require(invalid.graphics->efb->color == before.color && invalid.graphics->efb->depth == before.depth &&
                    invalid.graphics->efb->defined == before.defined && invalid.graphics->draws == 0 &&
                    invalid.graphics->decodedCommands == 0 && invalid.graphics->commands[0] == 0x80,
                "Rejected draw mutated pixels/counters or consumed its command.");
        if (failure == 0) {
            invalid.graphics->bp[0xc0] = 0x8f8af;
            ppc_write_u16(invalid, 0xcc000002, 0x11);
            Require(invalid.graphics->draws == 1 && invalid.graphics->rasterizedPixels == 16 &&
                        invalid.graphics->commands.empty(),
                    "Retry did not rasterize the retained complete quad exactly once.");
        }
    }
}

PpcState CopyState() {
    auto state = State();
    state.graphics->efb.emplace();
    auto& bp = state.graphics->bp;
    bp[0x49] = 2 | (2 << 10);
    bp[0x4a] = 3 | (1 << 10);
    bp[0x4b] = 0x800 >> 5;
    bp[0x4d] = 1;
    bp[0x4e] = 256;
    bp[0x53] = (32 << 12) | (32 << 18);
    bp[0x4f] = 0xff40;
    bp[0x50] = 0x4040;
    bp[0x51] = 0xabcdef;
    bp[0x40] = 0x1f;
    bp[0x41] = 8;
    return state;
}

void TestEfbCopies() {
    auto state = CopyState();
    auto& efb = *state.graphics->efb;
    auto& bp = state.graphics->bp;
    NativeEfbCopy(state, 0x4803);
    Require(efb.copies == 1 && efb.clears == 1 && efb.undefinedCopies == 1 &&
                efb.lastCopy->undefinedPixels == 8 && efb.lastCopy->address == 0x800 &&
                efb.lastCopy->width == 4 && efb.lastCopy->height == 2 && efb.lastCopy->stride == 32,
            "Cold EFB copy fabricated defined pixels or lost its actual XFB address/extent.");
    const auto index = 2 * NativeEfb::width + 2;
    Require(efb.color[index] == 0x404040ff && efb.depth[index] == 0xabcdef &&
                efb.defined[index] == 3 && efb.defined[index - 1] == 0 &&
                efb.defined[index + NativeEfb::width + 4] == 0,
            "Copy-clear did not define only the requested RGB8/Z24 source rectangle.");
    bp[0x4b] = 0x900 >> 5;
    NativeEfbCopy(state, 0x4003);
    Require(efb.copies == 2 && efb.clears == 1 && efb.undefinedCopies == 1 &&
                efb.lastCopy->undefinedPixels == 0,
            "A copied real clear did not produce defined XFB pixels.");
    for (uint32_t row = 0; row < 2; ++row) {
        const auto address = 0x900u + row * 32;
        Require(ppc_read_u32(state, address) == 0x47804780 &&
                    ppc_read_u32(state, address + 4) == 0x47804780 &&
                    state.memory[address - 1] == 0xa5 && state.memory[address + 8] == 0xa5,
                "Grey RGB8-to-YUYV copy differs or overwrote XFB row/padding guards.");
    }
    const uint32_t colors[] = {0xff0000ff, 0xff0000ff, 0xffffffff, 0xffffffff,
                                0x000000ff, 0x000000ff, 0x0000ffff, 0x0000ffff};
    for (unsigned pixel = 0; pixel < 8; ++pixel) {
        efb.color[index + (pixel / 4) * NativeEfb::width + pixel % 4] = colors[pixel];
    }
    NativeEfbCopy(state, 0x4003);
    Require(ppc_read_u32(state, 0x900) == 0x525a52f0 &&
                ppc_read_u32(state, 0x904) == 0xeb80eb80 &&
                ppc_read_u32(state, 0x920) == 0x10801080 &&
                ppc_read_u32(state, 0x924) == 0x29f0296e,
            "Primary/white/black RGB-to-limited-range Y0/Cb/Y1/Cr byte order differs.");
    bp[0x41] = 0;
    bp[0x40] = 0;
    bp[0x4f] = bp[0x50] = bp[0x51] = 0;
    NativeEfbCopy(state, 0x4803);
    Require(efb.color[index] == colors[0] && efb.depth[index] == 0xabcdef,
            "EFB clear ignored the color/depth update masks.");

    auto filtered = CopyState();
    auto& f = *filtered.graphics->efb;
    auto& registers = filtered.graphics->bp;
    registers[0x53] = 16 | (32 << 12);
    registers[0x54] = 16 << 6;
    for (unsigned row = 0; row < 4; ++row) {
        for (unsigned col = 0; col < 4; ++col) {
            const auto offset = (row + 1) * NativeEfb::width + col + 2;
            const uint32_t grey = row * 64;
            f.color[offset] = (grey << 24) | (grey << 16) | (grey << 8) | 255;
            f.defined[offset] = 1;
        }
    }
    NativeEfbCopy(filtered, 0x4003);
    Require(ppc_read_u32(filtered, 0x800) == 0x55805580 &&
                ppc_read_u32(filtered, 0x820) == 0x70807080,
            "Clamped three-row copy filter differs at top/bottom source edges.");
    NativeEfbCopy(filtered, 0x4000);
    Require(ppc_read_u32(filtered, 0x800) == 0x47804780 &&
                ppc_read_u32(filtered, 0x820) == 0x7e807e80,
            "Unclamped copy filter did not read actual adjacent EFB rows.");
    f.defined[NativeEfb::width + 2] = 0;
    NativeEfbCopy(filtered, 0x4000);
    Require(f.lastCopy->undefinedPixels == 2,
            "Copy filter did not propagate an undefined tap to its shared-chroma pixel pair.");
    for (unsigned masks = 0; masks < 4; ++masks) {
        auto masked = CopyState();
        masked.graphics->bp[0x41] = (masks & 1u) ? 8 : 0;
        masked.graphics->bp[0x40] = (masks & 2u) ? 16 : 0;
        NativeEfbCopy(masked, 0x4c03);
        const auto& pixels = *masked.graphics->efb;
        Require(pixels.defined[index] == masks &&
                    pixels.color[index] == ((masks & 1u) ? 0x404040ffu : 0xcdcdcdffu) &&
                    pixels.depth[index] == ((masks & 2u) ? 0xabcdefu : 0xcdcdcdu),
                "Unity inverse-scale copy/clear did not preserve independent RGB/depth masks and provenance.");
    }
}

void TestEfbFailuresAndRetry() {
    const auto initial = CopyState();
    for (unsigned test = 0; test < 18; ++test) {
        auto state = initial;
        auto& bp = state.graphics->bp;
        uint32_t control = 0x4803;
        const char* reason = "";
        switch (test) {
        case 0: control = 0x803; reason = "texture EFB"; break;
        case 1: control |= 0x80; reason = "gamma 1"; break;
        case 2: bp[0x43] = 1; reason = "RGB8/Z24"; break;
        case 3: bp[0x4e] = 128; reason = "vertical scaling"; break;
        case 4: bp[0x49] = 639; reason = "rectangle"; break;
        case 5: bp[0x4d] = 0; reason = "stride"; break;
        case 6: bp[0x4b] = 0x2000 >> 5; reason = "outside guest memory"; break;
        case 7: bp[0x4b] = 0x1000 >> 5; reason = "command FIFO"; break;
        case 8: bp[0x53] = 63 | (63 << 6); reason = "filter overflow"; break;
        case 9: bp[0x4f] = 0x10000; reason = "clear configuration"; break;
        case 10: bp[0] = 0x200; reason = "non-AA"; break;
        case 11: control |= 0x1000; reason = "field/half scaling"; break;
        case 12: bp[0x4a] = 2 | (1 << 10); reason = "odd X/width"; break;
        case 13: bp[0x49] = 2 | (527 << 10); reason = "rectangle"; break;
        case 14: state.graphics->efb->color.pop_back(); reason = "storage dimensions"; break;
        case 15: bp[0x4b] = 1 << 21; reason = "address/stride bits"; break;
        case 16: bp[0x4d] = 1 << 10; reason = "address/stride bits"; break;
        case 17: bp[0x54] = 1 << 18; reason = "filter overflow"; break;
        }
        const auto memory = state.memory;
        const auto before = *state.graphics->efb;
        Reject([&] { NativeEfbCopy(state, control); }, reason);
        const auto& efb = *state.graphics->efb;
        Require(state.memory == memory && efb.color == before.color &&
                    efb.depth == before.depth &&
                    efb.defined == before.defined && efb.copies == 0 &&
                    efb.clears == 0 && !efb.lastCopy,
                "Rejected EFB request partially wrote RAM, cleared storage or counted success.");
    }
    auto retry = CopyState();
    retry.graphics->efb.reset();
    Reject([&] { Send(retry, {0x61, 0x48, 0, 0x12, 0x34, 0x61, 0x52, 0, 0x48, 3}); }, "renderer/copy");
    Require(retry.graphics->decodedCommands == 1 && retry.graphics->commands[1] == 0x52 &&
                retry.graphics->fifo[4] == 0 && retry.graphics->fifo[6] == 0x1020 &&
                retry.graphics->token == 0x1234,
            "Failed copy replayed a prior token or staged the same FIFO burst twice.");
    retry.graphics->efb.emplace();
    ppc_write_u16(retry, 0xcc000002, 0x11);
    Require(retry.graphics->efb->copies == 1 && retry.graphics->efb->clears == 1 &&
                retry.graphics->commands.empty() && retry.graphics->decodedCommands == 24 &&
                retry.graphics->bursts == 1 && retry.graphics->fifo[6] == 0x1020,
            "Retry did not consume exactly one staged copy without replaying the token/FIFO burst.");
    auto committed = CopyState();
    Reject([&] { Send(committed, {0x61, 0x52, 0, 0x48, 3, 0x61, 0x65, 0, 0, 1}); }, "TLUT-memory");
    Reject([&] { ppc_write_u16(committed, 0xcc000002, 0x11); }, "TLUT-memory");
    Require(committed.graphics->efb->copies == 1 && committed.graphics->efb->clears == 1 &&
                committed.graphics->decodedCommands == 1 && committed.graphics->commands[1] == 0x65 &&
                committed.graphics->fifo[4] == 0 && committed.graphics->fifo[6] == 0x1020,
            "Retry after a later DMA rejection replayed an already committed EFB copy/clear.");
}

void TestTexturePreloadValidation() {
    auto state = State();
    Send(state, {0x61, 0x60, 0, 0, 0x40,
                 0x61, 0x61, 0, 0, 1,
                 0x61, 0x62, 0, 0, 2,
                 0x61, 0x63, 0, 0x80, 2});
    Require(state.graphics->texturePreloads == 1 &&
                state.graphics->texturePreloadBytes == 64 &&
                state.graphics->bp[0x63] == 0x8002 &&
                state.graphics->decodedCommands >= 4 &&
                state.graphics->commands.empty(),
            "Valid native texture preload did not validate its RAM source and retain its BP state.");

    auto invalid = State();
    Reject([&] {
        Send(invalid, {0x61, 0x60, 0, 1, 0,
                       0x61, 0x63, 0, 0, 1});
    }, "outside guest memory");
    Require(invalid.graphics->texturePreloads == 0 &&
                invalid.graphics->bp[0x63] == 0 &&
                invalid.graphics->commands[0] == 0x61 &&
                invalid.graphics->commands[1] == 0x63,
            "Rejected texture preload committed its BP register or transfer count.");
    invalid.graphics->bp[0x60] = 0x40;
    ppc_write_u16(invalid, 0xcc000002, 0x11);
    Require(invalid.graphics->texturePreloads == 1 &&
                invalid.graphics->texturePreloadBytes == 32 &&
                invalid.graphics->commands.empty(),
            "Correcting an invalid texture source did not retry the retained preload once.");
}

} // namespace

int main() {
    try {
        TestGatherAndCommands();
        TestInterruptsAndReader();
        TestBurstBoundaries();
        TestExplicitBoundaries();
        TestQuadRenderer();
        TestEfbCopies();
        TestEfbFailuresAndRetry();
        TestTexturePreloadValidation();
    } catch (const std::exception& error) {
        std::cerr << "Native GX test failure: " << error.what() << '\n';
        return 1;
    }
    std::cout << "Native GX FIFO RAM/gather, CP/BP/XF commands, ring wrap, interrupt sources "
                 "RGB5A3 quad pixels/coverage/blend/depth/scissor/atomic rejection, "
                 "checked RGB8/Z24 EFB clear/YUYV XFB copy/undefined-pixel provenance, and "
                 "validated texture-preload RAM sources passed.\n";
}
