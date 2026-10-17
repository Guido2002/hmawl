#include "generated_us_rel.h"
#include "us_module_fixture.h"
#include "native_time_base.h"
#include "disc_import.h"
#include "clz_image.h"
#include "native_dvd_reader.h"
#include "native_dvd_queue.h"
#include "sha1.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void Reject(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception& error) {
        if (std::string(error.what()).find(message) == std::string::npos) {
            throw std::runtime_error("Unexpected original native REL rejection: " +
                                     std::string(error.what()) + "; expected: " + message);
        }
        return;
    }
    throw std::runtime_error("Expected original native REL rejection: " + message);
}

void TestOriginalFunctions(const std::filesystem::path& directory) {
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    const auto translations = UsRelTranslations();
    NativeDispatcher dispatcher;
    dispatcher.RegisterRel(fixture.images[0], fixture.modules[0], state, translations);
    constexpr uint32_t object = 0x80001000u;
    constexpr uint32_t sentinel = 0x80003000u;
    ppc_write_u32(state, 0x803491c4u, object);
    for (uint32_t value : {0u, 1u, 0x80000000u, 0xffffffffu}) {
        ppc_write_u32(state, object + 0x48, value);
        state.lr = sentinel;
        state.gpr[1] = 0x8035cfa0u;
        state.gpr[31] = 0x12345678u;
        state.cr = 0x87654321u;
        dispatcher.Execute(state, 0x81000194u, 1, 1);
        Require(state.gpr[3] == (value == 0 ? 1u : 0u) && state.pc == sentinel &&
                    state.lr == sentinel && state.gpr[1] == 0x8035cfa0u &&
                    state.gpr[31] == 0x12345678u && state.cr == 0x87654321u,
                "Original REL zero-test function result or ABI mismatch.");
        ppc_write_u32(state, object + 0x1c0, value);
        dispatcher.Execute(state, 0x810001f0u, 1, 1);
        Require(state.gpr[3] == value && state.pc == sentinel && state.lr == sentinel,
                "Original REL field getter result or return mismatch.");
    }
    auto moved = fixture.modules[0];
    moved.sectionAddresses[1] += 0x1000;
    NativeDispatcher invalid;
    Reject([&] { invalid.RegisterRel(fixture.images[0], moved, state, translations); },
           "compiled placement");
    Require(!invalid.Contains(0x81000194u), "Failed native REL binding partially committed.");
    auto bad = translations;
    bad[1].offset += 4;
    Reject([&] { invalid.RegisterRel(fixture.images[0], fixture.modules[0], state, bad); },
           "compiled placement");
    bad = translations;
    bad[0].section = 2;
    Reject([&] { invalid.RegisterRel(fixture.images[0], fixture.modules[0], state, bad); },
           "executable range");
    bad = translations;
    bad[0].section = 256;
    Reject([&] { invalid.RegisterRel(fixture.images[0], fixture.modules[0], state, bad); },
           "invalid section");
    moved.id = 2;
    Reject([&] { invalid.RegisterRel(fixture.images[0], moved, state, translations); },
           "does not match");
    const auto firstWord = ppc_read_u32(state, 0x81000194u);
    ppc_write_u32(state, 0x81000194u, 0);
    Reject([&] { dispatcher.Execute(state, 0x81000194u); }, "code mismatch");
    ppc_write_u32(state, 0x81000194u, firstWord);
    dispatcher.Unregister(1);
    Reject([&] { dispatcher.Execute(state, 0x81000194u); }, "No native translation");
    dispatcher.RegisterRel(fixture.images[0], fixture.modules[0], state, translations);
    dispatcher.Execute(state, 0x81000194u);
    Require(state.pc == sentinel, "Original REL native re-registration failed.");
}

constexpr uint32_t kStack = 0x8035cfa0u;
constexpr uint32_t kReturn = 0x80003000u;

void PrepareCall(PpcState& state) {
    for (uint32_t reg = 0; reg < 32; ++reg) {
        state.gpr[reg] = 0xa5000000u + reg;
    }
    state.gpr[1] = kStack;
    state.gpr[2] = 0x80351e40u;
    state.gpr[13] = 0x8034e9c0u;
    state.lr = kReturn;
}

void CheckReturn(const PpcState& state) {
    Require(state.pc == kReturn && state.lr == kReturn &&
                state.gpr[1] == kStack && state.gpr[2] == 0x80351e40u &&
                state.gpr[13] == 0x8034e9c0u,
            "Original module initialization did not preserve return/stack/SDA state.");
    for (uint32_t reg = 14; reg < 32; ++reg) {
        Require(state.gpr[reg] == 0xa5000000u + reg,
                "Original module initialization did not restore a nonvolatile GPR.");
    }
}

void WriteGuestString(PpcState& state, uint32_t address, const std::string& text) {
    const auto offset = ppc_memory_offset(state, address, text.size() + 1);
    std::copy(text.begin(), text.end(), state.memory.begin() + offset);
    state.memory[offset + text.size()] = 0;
}

void CheckCopy(const PpcState& state, uint32_t destination,
               uint32_t source, size_t size) {
    const auto from = ppc_memory_offset(state, source, size);
    const auto to = ppc_memory_offset(state, destination, size);
    Require(std::equal(state.memory.begin() + from, state.memory.begin() + from + size,
                       state.memory.begin() + to),
            "Original static constructor table copy differs from its source.");
}

void CheckConstructorData(const UsModuleFixture& fixture) {
    const auto& state = fixture.state;
    CheckCopy(state, 0x81031030u, 0x81030fd0u, 0x60);
    CheckCopy(state, 0x810310d8u, 0x810310a8u, 0x30);
    CheckCopy(state, 0x81031140u, 0x81031110u, 0x30);
    CheckCopy(state, 0x810322d0u, 0x81032270u, 0x60);
    CheckCopy(state, 0x810333d8u, 0x8025116cu, 12);
    CheckCopy(state, 0x810333e4u, 0x80251178u, 12);
    CheckCopy(state, 0x810333f0u, 0x80251160u, 12);
    CheckCopy(state, 0x810333fcu, 0x80251154u, 12);
    CheckCopy(state, 0x810373c0u, 0x81035e60u, 12);
    const auto bss = fixture.modules[0].sectionAddresses[6];
    Require(ppc_read_u32(state, bss + 0x4140) == bss + 0x10 &&
                ppc_read_u32(state, bss + 0x10) == 0 &&
                ppc_read_u32(state, bss + 0x14) == 0x8101301cu &&
                ppc_read_u32(state, bss + 0x18) == bss + 0x1c &&
                ppc_read_u32(state, bss + 0x1c) == 0x802ab7f8u &&
                ppc_read_u32(state, bss + 0x20) == 0 &&
                ppc_read_u32(state, bss + 0x24) == 0 &&
                ppc_read_u8(state, bss + 0x28) == 0 &&
                ppc_read_u32(state, bss + 0x2c) == 0,
            "Original global object construction/destructor registration differs.");
}

void CheckInitializationWrites(const UsModuleFixture& fixture,
                                const std::vector<uint8_t>& before,
                                std::vector<RelMemoryRange> ranges = {}) {
    std::vector<std::pair<size_t, size_t>> writable;
    for (const auto& range : std::vector<RelMemoryRange>{
             {fixture.modules[0].sectionAddresses[5], fixture.images[0].sections()[5].size},
             {fixture.modules[0].sectionAddresses[6], fixture.images[0].sections()[6].size},
             {kStack - 0x500, 0x508}}) {
        ranges.push_back(range);
    }
    for (const auto& range : ranges) {
        const auto offset = ppc_memory_offset(fixture.state, range.address, range.size);
        writable.push_back({offset, offset + range.size});
    }
    std::sort(writable.begin(), writable.end());
    const auto check = [&](size_t first, size_t last) {
        const auto mismatch = std::mismatch(before.begin() + first, before.begin() + last,
                                            fixture.state.memory.begin() + first);
        if (mismatch.first != before.begin() + last) {
            std::ostringstream error;
            error << "Original initialization wrote outside its permitted memory ranges at RAM offset 0x"
                  << std::hex << std::distance(before.begin(), mismatch.first) << ".";
            throw std::runtime_error(error.str());
        }
    };
    size_t cursor = 0;
    for (const auto& range : writable) {
        check(cursor, range.first);
        cursor = range.second;
    }
    check(cursor, before.size());
}

void TestOriginalInitialization(const std::filesystem::path& directory) {
    auto reference = LoadUsModuleFixture(directory);
    NativeDispatcher dispatcher;
    dispatcher.Register(0, reference.state, UsRelDolTranslations());
    dispatcher.RegisterRel(reference.images[0], reference.modules[0], reference.state,
                           UsRelTranslations());
    const auto before = reference.state.memory;
    const std::array<uint32_t, 13> constructors = {
        0x81001f24u, 0x81003d48u, 0x81007184u, 0x8100ad84u, 0x8100d300u,
        0x8100e56cu, 0x81010628u, 0x81013164u, 0x810198d8u, 0x8101d414u,
        0x8101da20u, 0x81020ac0u, 0x8102adb8u};
    const auto ctorTable = reference.modules[0].sectionAddresses[2];
    for (size_t i = 0; i < constructors.size(); ++i) {
        Require(ppc_read_u32(reference.state, ctorTable + static_cast<uint32_t>(i * 4)) ==
                    constructors[i], "Original constructor array differs from the verified manifest.");
        const auto previous = reference.state.memory;
        PrepareCall(reference.state);
        dispatcher.Execute(reference.state, constructors[i]);
        CheckReturn(reference.state);
        const auto& data = reference.images[0].sections()[5];
        const auto offset = ppc_memory_offset(reference.state,
            reference.modules[0].sectionAddresses[5], data.size);
        Require(!std::equal(previous.begin() + offset, previous.begin() + offset + data.size,
                            reference.state.memory.begin() + offset),
                "Original static constructor did not initialize its data.");
    }
    Require(ppc_read_u32(reference.state, ctorTable + 13 * 4) == 0,
            "Original constructor array is not terminated.");
    CheckConstructorData(reference);
    CheckInitializationWrites(reference, before);

    for (uint32_t mode : {0u, 8u, 9u}) {
        auto fixture = LoadUsModuleFixture(directory);
        auto& state = fixture.state;
        constexpr uint32_t system = 0x80001000u;
        ppc_write_u32(state, 0x80349208u, system);
        ppc_write_u32(state, system + 0x5c, mode);
        ppc_write_u32(state, system + 0x4c, 0xdeadbeefu);
        const auto initial = state.memory;
        PrepareCall(state);
        Reject([&] { dispatcher.Execute(state, fixture.modules[0].prolog); },
               "No native translation registered at 0x80178E88");
        if (!(state.pc == 0x80178e88u &&
                    state.gpr[3] == (mode == 8 ? 0x60u : mode == 9 ? 0xb8u : 0x44u) &&
                    state.gpr[5] == (mode == 8 ? 0x5du : mode == 9 ? 0x5cu : 0x5bu) &&
                    ppc_read_u32(state, system + 0x4c) == 0xdeadbeefu &&
                    state.gpr[1] == kStack - 0x20)) {
            std::ostringstream error;
            error << "Original prolog allocator boundary mismatch: mode " << mode
                  << ", PC=0x" << std::hex << state.pc << ", size=0x" << state.gpr[3]
                  << ", line=0x" << state.gpr[5] << ", SP=0x" << state.gpr[1]
                  << ", object field=0x" << ppc_read_u32(state, system + 0x4c) << ".";
            throw std::runtime_error(error.str());
        }
        CheckConstructorData(fixture);
        CheckInitializationWrites(fixture, initial);
        for (const size_t section : {size_t{5}, size_t{6}}) {
            const auto& info = fixture.images[0].sections()[section];
            const auto offset = ppc_memory_offset(state,
                fixture.modules[0].sectionAddresses[section], info.size);
            Require(std::equal(reference.state.memory.begin() + offset,
                               reference.state.memory.begin() + offset + info.size,
                               state.memory.begin() + offset),
                    "Prolog constructor array did not produce the standalone chain's data/BSS.");
        }
        PrepareCall(state);
        dispatcher.Execute(state, fixture.modules[0].epilog);
        CheckReturn(state);
        const auto bss = fixture.modules[0].sectionAddresses[6];
        Require(ppc_read_u32(state, bss + 0x4140) == 0 &&
                    ppc_read_u32(state, bss + 0x1c) == 0x81032d1cu,
                "Original epilog did not remove and invoke the registered destructor.");
        const auto afterDestruction = state.memory;
        PrepareCall(state);
        dispatcher.Execute(state, fixture.modules[0].epilog);
        CheckReturn(state);
        const auto offset = ppc_memory_offset(state, bss, fixture.images[0].sections()[6].size);
        Require(std::equal(afterDestruction.begin() + offset,
                           afterDestruction.begin() + offset + fixture.images[0].sections()[6].size,
                           state.memory.begin() + offset),
                "Original epilog changed BSS when its destructor chain was empty.");
        CheckInitializationWrites(fixture, initial);
    }
}

constexpr uint32_t kArenaBottom = 0x80600000u;
constexpr uint32_t kArenaSize = 0x10000u;
constexpr uint32_t kHeap = 0x80315508u;
constexpr uint32_t kNodes = 0x802fd4fcu;

void InitializeOriginalHeap(PpcState& state, NativeDispatcher& dispatcher) {
    for (const auto& array : std::array<std::array<uint32_t, 3>, 2>{{
             {0x802fc8c4u, 128, 0x80179ee8u},
             {kNodes, 4096, 0x80179c90u}}}) {
        for (uint32_t i = 0; i < array[1]; ++i) {
            PrepareCall(state);
            state.gpr[3] = array[0] + i * 24;
            dispatcher.Execute(state, array[2]);
        }
    }
    PrepareCall(state);
    state.gpr[3] = kArenaBottom;
    state.gpr[4] = kArenaSize;
    state.gpr[5] = kArenaBottom + kArenaSize;
    state.gpr[6] = kArenaSize;
    dispatcher.Execute(state, 0x80178558u);
    CheckReturn(state);
    Require(ppc_read_u32(state, kHeap) == kArenaBottom + kArenaSize &&
                ppc_read_u32(state, kHeap + 4) == kArenaSize &&
                ppc_read_u32(state, kHeap + 8) == kNodes &&
                ppc_read_u16(state, kHeap + 12) == 4096 &&
                ppc_read_u32(state, kHeap + 16) == kNodes &&
                ppc_read_u32(state, kHeap + 20) == kNodes &&
                ppc_read_u32(state, kHeap + 24) == kNodes + 24,
            "Original Memory::InitializeWithArena heap layout mismatch.");
    for (uint32_t i = 0; i < 4096; ++i) {
        const uint32_t node = kNodes + i * 24;
        Require(ppc_read_u32(state, node + 20) == 0x802c9d04u &&
                    ppc_read_u32(state, node + 12) == (i == 0 ? 1u : 0u) &&
                    ppc_read_u32(state, node + 8) ==
                        (i == 4095 || i == 0 ? 0u : node + 24),
                "Original heap metadata initialization/vtable/free chain mismatch.");
    }
}

void TestOriginalAllocator(const std::filesystem::path& directory) {
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsRelDolTranslations());
    dispatcher.Register(100, state, UsAllocatorTranslations());
    dispatcher.RegisterRel(fixture.images[0], fixture.modules[0], state, UsRelTranslations());
    InitializeOriginalHeap(state, dispatcher);
    uint64_t tick = 0x12345678abcdef00ull;
    state.readTimeBase = [&tick] { return ++tick; };
    uint32_t remaining = kArenaSize;
    uint32_t nodeIndex = 0;
    uint32_t previousNode = 0;
    uint32_t expectedCount = 0;
    const auto initialCode = state.memory;
    for (const uint32_t size : {0u, 1u, 31u, 32u, 33u, 0x44u, 0x60u, 0xb8u}) {
        PrepareCall(state);
        state.gpr[3] = size;
        state.gpr[4] = 0x81033478u;
        state.gpr[5] = 91;
        const auto unchanged = state.memory;
        dispatcher.Execute(state, 0x80178e88u);
        CheckReturn(state);
        ++expectedCount;
        Require(ppc_read_u32(state, 0x80349494u) == expectedCount &&
                    ppc_read_u32(state, 0x80349498u) == expectedCount,
                "Original new did not update allocation/high-water counters.");
        if (size == 0) {
            Require(state.gpr[3] == 0 && tick == 0x12345678abcdef00ull,
                    "Original zero-sized allocation touched heap or read the clock.");
            const auto start = ppc_memory_offset(state, kNodes, 4096 * 24);
            Require(std::equal(unchanged.begin() + start, unchanged.begin() + start + 4096 * 24,
                               state.memory.begin() + start),
                    "Zero-sized allocation modified heap metadata.");
            continue;
        }
        const uint32_t aligned = (size + 31) & ~31u;
        remaining -= aligned;
        const auto node = kNodes + nodeIndex * 24;
        const auto next = node + 24;
        Require(state.gpr[3] == kArenaBottom + remaining &&
                    (state.gpr[3] & 31u) == 0 &&
                    ppc_read_u32(state, node + 4) == aligned &&
                    ppc_read_u32(state, node + 8) == next &&
                    ppc_read_u32(state, node + 12) == 2 &&
                    ppc_read_u32(state, node + 16) == static_cast<uint32_t>(tick) &&
                    ppc_read_u32(state, next + 4) == remaining &&
                    ppc_read_u32(state, next + 12) == 1 &&
                    ppc_read_u32(state, kHeap + 20) == next,
                "Original split allocation address, metadata, timestamp or tail pointer mismatch.");
        previousNode = node;
        ++nodeIndex;
    }
    PrepareCall(state);
    state.gpr[3] = remaining;
    dispatcher.Execute(state, 0x80178e88u);
    CheckReturn(state);
    const auto lastNode = kNodes + nodeIndex * 24;
    Require(state.gpr[3] == kArenaBottom &&
                ppc_read_u32(state, lastNode + 4) == remaining &&
                ppc_read_u32(state, lastNode + 8) == 0 &&
                ppc_read_u32(state, lastNode + 12) == 2 &&
                ppc_read_u32(state, kHeap + 24) == lastNode + 24 &&
                ppc_read_u32(state, previousNode + 8) == lastNode,
            "Original exact-fit allocation consumed metadata or returned the wrong address.");
    const auto exhausted = state.memory;
    PrepareCall(state);
    state.gpr[3] = 32;
    state.gpr[4] = 0x81033478u;
    state.gpr[5] = 123;
    Reject([&] { dispatcher.Execute(state, 0x80178e88u); },
           "No native translation registered at 0x801770A8");
    Require(state.gpr[4] == 0x4d7 && state.gpr[6] == 0x81033478u &&
                state.gpr[7] == 123 && state.gpr[8] == 32,
            "Original out-of-memory path lost its panic/source/size arguments.");
    const auto start = ppc_memory_offset(state, kNodes, 4096 * 24);
    Require(std::equal(exhausted.begin() + start, exhausted.begin() + start + 4096 * 24,
                       state.memory.begin() + start),
            "Failed original allocation modified its metadata.");
    const auto arena = ppc_memory_offset(state, kArenaBottom, kArenaSize);
    Require(std::equal(initialCode.begin() + arena, initialCode.begin() + arena + kArenaSize,
                       state.memory.begin() + arena),
            "Original allocator unexpectedly modified allocated payload bytes.");

    {
        auto limited = LoadUsModuleFixture(directory);
        auto& guest = limited.state;
        InitializeOriginalHeap(guest, dispatcher);
        guest.readTimeBase = [&tick] { return ++tick; };
        ppc_write_u32(guest, kHeap + 24, 0);
        PrepareCall(guest);
        guest.gpr[3] = 32;
        Reject([&] { dispatcher.Execute(guest, 0x80178e88u); },
               "No native translation registered at 0x801770A8");
        Require(ppc_read_u32(guest, kNodes + 4) == kArenaSize &&
                    ppc_read_u32(guest, kNodes + 12) == 1,
                "Original allocator consumed a block without a split descriptor.");
    }
    {
        auto noClock = LoadUsModuleFixture(directory);
        auto& guest = noClock.state;
        InitializeOriginalHeap(guest, dispatcher);
        PrepareCall(guest);
        guest.gpr[3] = 32;
        Reject([&] { dispatcher.Execute(guest, 0x80178e88u); },
               "No native PowerPC time-base source configured");
        Require(guest.pc == 0x801b7c08u,
                "Original allocator hid its missing time-base dependency.");
    }

    for (const bool reverse : {false, true}) {
        auto lifetime = LoadUsModuleFixture(directory);
        auto& guest = lifetime.state;
        InitializeOriginalHeap(guest, dispatcher);
        guest.readTimeBase = [&tick] { return ++tick; };
        std::array<uint32_t, 3> allocated = {};
        for (size_t i = 0; i < allocated.size(); ++i) {
            PrepareCall(guest);
            guest.gpr[3] = 32;
            dispatcher.Execute(guest, 0x80178e88u);
            CheckReturn(guest);
            allocated[i] = guest.gpr[3];
            ppc_write_u32(guest, allocated[i], 0x12340000u + static_cast<uint32_t>(i));
        }
        const std::array<size_t, 3> order = reverse ?
            std::array<size_t, 3>{2, 1, 0} : std::array<size_t, 3>{1, 0, 2};
        uint32_t live = 3;
        for (const auto index : order) {
            PrepareCall(guest);
            guest.gpr[3] = allocated[index];
            dispatcher.Execute(guest, 0x80179168u);
            CheckReturn(guest);
            Require(ppc_read_u32(guest, 0x80349494u) == --live &&
                        ppc_read_u32(guest, 0x80349498u) == 3 &&
                        ppc_read_u32(guest, allocated[index]) == 0x12340000u + index,
                    "Original delete counters, high-water mark or payload preservation failed.");
        }
        const uint32_t head = ppc_read_u32(guest, kHeap + 16);
        if (!(ppc_read_u32(guest, head + 4) == kArenaSize &&
              ppc_read_u32(guest, head + 8) == 0 &&
              ppc_read_u32(guest, head + 12) == 1)) {
            std::ostringstream error;
            error << "Original coalescing mismatch: reverse=" << reverse << std::hex
                  << ", head=" << head << ", tail=" << ppc_read_u32(guest, kHeap + 20)
                  << ", size=" << ppc_read_u32(guest, head + 4)
                  << ", next=" << ppc_read_u32(guest, head + 8)
                  << ", status=" << ppc_read_u32(guest, head + 12);
            throw std::runtime_error(error.str());
        }
        PrepareCall(guest);
        guest.gpr[3] = 0;
        dispatcher.Execute(guest, 0x80179168u);
        CheckReturn(guest);
        Require(ppc_read_u32(guest, 0x80349494u) == 0,
                "Original null delete changed allocation count.");
        PrepareCall(guest);
        guest.gpr[3] = kArenaSize;
        dispatcher.Execute(guest, 0x80178e88u);
        CheckReturn(guest);
        Require(guest.gpr[3] == kArenaBottom, "Original heap could not reuse its coalesced arena.");
    }

    for (uint32_t mode : {0u, 8u, 9u}) {
        auto moduleFixture = LoadUsModuleFixture(directory);
        auto& guest = moduleFixture.state;
        InitializeOriginalHeap(guest, dispatcher);
        guest.readTimeBase = MakeNativeTimeBase();
        constexpr uint32_t system = 0x80001000u;
        ppc_write_u32(guest, 0x80349208u, system);
        ppc_write_u32(guest, system + 0x5c, mode);
        ppc_write_u32(guest, system + 0x4c, 0xdeadbeefu);
        PrepareCall(guest);
        const auto target = mode == 8 ? 0x810042dcu : mode == 9 ? 0x81008a94u : 0x810025ccu;
        std::ostringstream missing;
        missing << "No native translation registered at 0x" << std::hex << std::uppercase << target;
        Reject([&] { dispatcher.Execute(guest, moduleFixture.modules[0].prolog); }, missing.str());
        const uint32_t aligned = mode == 9 ? 0xc0u : 0x60u;
        Require(guest.pc == target &&
                    guest.gpr[3] == kArenaBottom + kArenaSize - aligned &&
                    ppc_read_u32(guest, kNodes + 4) == aligned &&
                    ppc_read_u32(guest, kNodes + 12) == 2 &&
                    ppc_read_u32(guest, system + 0x4c) == 0xdeadbeefu,
                "Original module prolog did not allocate and reach its real scene constructor.");
        CheckConstructorData(moduleFixture);
    }
}

void TestSceneServices(const std::filesystem::path& directory) {
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsRelDolTranslations());
    dispatcher.Register(100, state, UsAllocatorTranslations());
    dispatcher.Register(101, state, UsSceneDependencyTranslations());
    constexpr std::array<uint32_t, 2> managers = {0x80001000u, 0x80001200u};
    constexpr uint32_t nodes = 0x80002000u;
    constexpr uint32_t calendar = 0x80002800u;
    for (const auto manager : managers) {
        const auto offset = ppc_memory_offset(state, manager, 0xa0);
        std::fill(state.memory.begin() + offset, state.memory.begin() + offset + 0xa0, 0xa5);
        const auto before = state.memory;
        PrepareCall(state);
        state.gpr[3] = manager;
        dispatcher.Execute(state, 0x8000b4c4u);
        CheckReturn(state);
        Require(state.gpr[3] == manager &&
                    ppc_read_u32(state, 0x8034923cu) == manager &&
                    ppc_read_u32(state, manager + 0x28) == 0x80297a98u &&
                    ppc_read_u32(state, manager + 4) == manager + 0x14 &&
                    ppc_read_u32(state, manager + 0x14) == manager &&
                    ppc_read_u32(state, manager + 0x18) == 0 &&
                    ppc_read_u32(state, manager + 0x2c) == 0x80297b20u,
                "Original registration manager initialization mismatch.");
        for (uint32_t offset = 0x44; offset < 0x47; ++offset) {
            Require(ppc_read_u8(state, manager + offset) == 0,
                    "Original manager did not clear its three-byte bitset.");
        }
        CheckInitializationWrites(fixture, before, {{manager, 0xa0}, {0x8034923cu, 4}});
    }
    for (uint32_t i = 0; i < 6; ++i) {
        ppc_write_u32(state, nodes + i * 0x40 + 0xc, nodes + i * 0x40);
    }
    std::array<std::vector<uint32_t>, 2> expected;
    const auto beforeRegistration = state.memory;
    const auto insert = [&](size_t owner, uint32_t index, uint32_t priority) {
        const uint32_t node = nodes + index * 0x40;
        for (auto& list : expected) {
            list.erase(std::remove(list.begin(), list.end(), node), list.end());
        }
        auto& destination = expected[owner];
        const auto position = std::find_if(destination.begin(), destination.end(),
            [&](uint32_t existing) { return ppc_read_u32(state, existing + 0x14) >= priority; });
        destination.insert(position, node);
        PrepareCall(state);
        state.gpr[3] = managers[owner];
        state.gpr[4] = node;
        state.gpr[5] = priority;
        dispatcher.Execute(state, 0x8017cd5cu);
        CheckReturn(state);
        Require(ppc_read_u32(state, node + 0x14) == priority,
                "Original object registration lost its unsigned priority.");
        for (size_t list = 0; list < managers.size(); ++list) {
            const auto manager = managers[list];
            uint32_t previous = manager;
            for (const auto entry : expected[list]) {
                Require(ppc_read_u32(state, previous + 4) == entry &&
                            ppc_read_u32(state, entry) == previous &&
                            ppc_read_u32(state, entry + 8) == manager &&
                            ppc_read_u32(state, entry + 0xc) == entry,
                        "Original sorted registration/reparenting corrupted object list links.");
                previous = entry;
            }
            Require(ppc_read_u32(state, previous + 4) == manager + 0x14 &&
                        ppc_read_u32(state, manager + 0x14) == previous &&
                        ppc_read_u32(state, manager + 0x18) == 0 &&
                        ppc_read_u32(state, manager) == 0,
                    "Original registration corrupted its head or terminator links.");
        }
    };
    insert(0, 0, 0x3002);
    insert(0, 1, 0);
    insert(0, 2, 0xffffffffu);
    insert(0, 3, 0xa000);
    insert(0, 4, 0xa000);
    insert(0, 5, 1);
    insert(0, 2, 0);
    insert(1, 1, 0xffffffffu);
    insert(1, 3, 5);
    insert(1, 4, 5);
    insert(0, 3, 0x3002);
    insert(0, 4, 0x3002);
    for (uint32_t index = 0; index < 6; ++index) {
        insert(1, index, 6 - index);
    }
    CheckInitializationWrites(fixture, beforeRegistration, {
        {managers[0], 0xa0}, {managers[1], 0xa0}, {nodes, 6 * 0x40}});

    const std::array<uint32_t, 6> durations = {1, 2, 3, 2, 1, 1};
    for (uint32_t i = 0; i < durations.size(); ++i) {
        Require(ppc_read_u32(state, 0x8023df70u + i * 4) == durations[i],
                "Verified calendar duration table differs from the test reference.");
    }
    const auto beforeCalendar = state.memory;
    for (uint32_t day = 0; day <= 124; ++day) {
        for (uint32_t fraction : {0u, 34559999u}) {
            const uint64_t ticks = uint64_t{day} * 34560000u + fraction;
            if (ticks > UINT32_MAX) {
                continue;
            }
            ppc_write_u32(state, calendar, static_cast<uint32_t>(ticks));
            PrepareCall(state);
            state.gpr[3] = calendar;
            dispatcher.Execute(state, 0x8000fee8u);
            CheckReturn(state);
            const uint32_t expectedPeriod = day < 1 ? 0u : day < 3 ? 1u :
                day < 6 ? 2u : day < 8 ? 3u : day < 9 ? 4u : 5u;
            Require(state.gpr[3] == expectedPeriod,
                    "Original calendar period lookup failed a boundary or saturation case.");
        }
    }
    ppc_write_u32(state, calendar, UINT32_MAX);
    PrepareCall(state);
    state.gpr[3] = calendar;
    dispatcher.Execute(state, 0x8000fee8u);
    CheckReturn(state);
    Require(state.gpr[3] == 5, "Original calendar lookup did not clamp the maximum input.");
    CheckInitializationWrites(fixture, beforeCalendar, {{calendar, 4}});

    constexpr uint32_t archive = 0x80002a00u;
    PrepareCall(state);
    state.gpr[3] = archive;
    state.gpr[4] = 0;
    dispatcher.Execute(state, 0x80199eecu);
    CheckReturn(state);
    Require(state.gpr[3] == 1 && ppc_read_u32(state, archive + 0xc) == 0 &&
                ppc_read_u32(state, archive + 0x1c) == 0,
            "Original archive initializer rejected its empty-resource case.");
    PrepareCall(state);
    state.gpr[3] = archive;
    state.gpr[4] = 0x80700000u;
    Reject([&] { dispatcher.Execute(state, 0x80199eecu); },
           "No native translation registered at 0x80203B4C");
    Require(state.pc == 0x80203b4cu && state.gpr[3] == 0x80700000u &&
                state.gpr[4] == archive && ppc_read_u32(state, archive + 0x1c) == 0x80700000u,
            "Nonempty archive initialization bypassed its original ARC parser.");
}

void TestOriginalArchives(const std::filesystem::path& directory) {
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsSceneDependencyTranslations());
    dispatcher.Register(1, state, UsArchiveTranslations());
    constexpr uint32_t archive = 0x80700000u;
    constexpr uint32_t handle = 0x80001000u;
    constexpr uint32_t info = 0x80001200u;
    constexpr uint32_t path = 0x80001400u;
    constexpr uint32_t fst = archive + 0x20;
    constexpr uint32_t names = fst + 5 * 12;
    const auto writeString = [&](uint32_t address, const std::string& value) {
        for (size_t i = 0; i <= value.size(); ++i) {
            ppc_write_u8(state, address + static_cast<uint32_t>(i),
                         i == value.size() ? 0 : static_cast<uint8_t>(value[i]));
        }
    };
    ppc_write_u32(state, archive, 0x55aa382du);
    ppc_write_u32(state, archive + 4, 0x20);
    ppc_write_u32(state, archive + 8, 0x70);
    ppc_write_u32(state, archive + 12, 0x100);
    const std::array<std::array<uint32_t, 3>, 5> entries = {{
        {0x01000000u, 0, 5}, {0x01000001u, 0, 4},
        {5, 0x100, 4}, {15, 0x104, 0}, {25, 0x108, 3}}};
    for (uint32_t i = 0; i < entries.size(); ++i) {
        for (uint32_t word = 0; word < 3; ++word) {
            ppc_write_u32(state, fst + i * 12 + word * 4, entries[i][word]);
        }
    }
    writeString(names, "");
    writeString(names + 1, "dir");
    writeString(names + 5, "child.bin");
    writeString(names + 15, "empty.bin");
    writeString(names + 25, "root.bin");
    ppc_write_u32(state, archive + 0x100, 0x12345678u);
    ppc_write_u32(state, archive + 0x108, 0xa1b2c300u);
    const auto before = state.memory;
    PrepareCall(state);
    state.gpr[3] = handle;
    state.gpr[4] = archive;
    dispatcher.Execute(state, 0x80199eecu);
    CheckReturn(state);
    Require(state.gpr[3] == 1, "Original game archive initialization failed valid U8 input.");
    const std::array<uint32_t, 8> expectedHandle = {
        archive, fst, archive + 0x100, 5, names, 0x70, 0, archive};
    for (uint32_t i = 0; i < expectedHandle.size(); ++i) {
        Require(ppc_read_u32(state, handle + i * 4) == expectedHandle[i],
                "Original U8 handle pointers, entry count or resource owner mismatch.");
    }
    for (const auto& lookup : std::vector<std::pair<std::string, uint32_t>>{
             {"", 0}, {"/", 0}, {".", 0}, {"..", 0}, {"dir", 1}, {"dir/", 1},
             {"DIR/CHILD.BIN", 2}, {"/dir/./child.bin", 2},
             {"dir/../root.bin", 4}, {"//dir/empty.bin", 3},
             {"dir//empty.bin", UINT32_MAX},
             {"root.bin", 4}, {"root", UINT32_MAX}, {"missing", UINT32_MAX},
             {"child.bin", UINT32_MAX}, {"root.bin/nope", UINT32_MAX},
             {"dir/missing", UINT32_MAX}, {"dir/child.bin.extra", UINT32_MAX}}) {
        writeString(path, lookup.first);
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = path;
        dispatcher.Execute(state, 0x80203c4cu);
        CheckReturn(state);
        if (state.gpr[3] != lookup.second) {
            throw std::runtime_error("Original U8 path lookup mismatch for '" + lookup.first +
                "': expected " + std::to_string(lookup.second) + ", got " +
                std::to_string(state.gpr[3]) + ".");
        }
    }
    ppc_write_u32(state, handle + 0x18, 1);
    for (const auto& lookup : std::vector<std::pair<std::string, uint32_t>>{
             {"", 1}, {".", 1}, {"..", 0}, {"child.bin", 2},
             {"../root.bin", 4}, {"/root.bin", 4}, {"empty.bin", 3}}) {
        writeString(path, lookup.first);
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = path;
        dispatcher.Execute(state, 0x80203c4cu);
        CheckReturn(state);
        Require(state.gpr[3] == lookup.second, "Original relative U8 path lookup failed.");
    }
    ppc_write_u32(state, handle + 0x18, 0);
    for (uint32_t index : {0u, 1u, 2u, 3u, 4u, 5u, UINT32_MAX}) {
        for (uint32_t word = 0; word < 3; ++word) {
            ppc_write_u32(state, info + word * 4, 0xdeadbeefu);
        }
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = index;
        state.gpr[5] = info;
        dispatcher.Execute(state, 0x80203bf0u);
        CheckReturn(state);
        const bool valid = index >= 2 && index <= 4;
        Require(state.gpr[3] == (valid ? 1u : 0u),
                "Original U8 indexed open did not reject a directory or invalid index.");
        if (!valid) {
            Require(ppc_read_u32(state, info) == 0xdeadbeefu &&
                        ppc_read_u32(state, info + 4) == 0xdeadbeefu &&
                        ppc_read_u32(state, info + 8) == 0xdeadbeefu,
                    "Rejected U8 indexed open modified its output.");
            continue;
        }
        Require(ppc_read_u32(state, info) == handle &&
                    ppc_read_u32(state, info + 4) == entries[index][1] &&
                    ppc_read_u32(state, info + 8) == entries[index][2],
                "Original U8 indexed open produced incorrect file metadata.");
        for (const auto& getter : std::array<std::pair<uint32_t, uint32_t>, 2>{{
                 {0x80199f5cu, archive + entries[index][1]},
                 {0x80199fb8u, entries[index][2]}}}) {
            PrepareCall(state);
            state.gpr[3] = handle;
            state.gpr[4] = index;
            dispatcher.Execute(state, getter.first);
            CheckReturn(state);
            Require(state.gpr[3] == getter.second,
                    "Original game archive file pointer/length wrapper mismatch.");
        }
        if (index == 2) {
            Require(ppc_read_u32(state, archive + entries[index][1]) == 0x12345678u,
                    "Original U8 file pointer did not reference the expected payload.");
        }
    }
    CheckInitializationWrites(fixture, before, {{handle, 0x20}, {info, 12}, {path, 128}});
    ppc_write_u32(state, archive, 0);
    PrepareCall(state);
    state.gpr[3] = handle;
    state.gpr[4] = archive;
    Reject([&] { dispatcher.Execute(state, 0x80199eecu); },
           "No native translation registered at 0x801770A8");
    Require(state.pc == 0x801770a8u && state.gpr[4] == 0x3e,
            "Invalid U8 magic did not reach the original explicit panic path.");
    ppc_write_u32(state, archive, 0x55aa382du);
    ppc_write_u32(state, archive + 4, 0x01800000u);
    PrepareCall(state);
    state.gpr[3] = archive;
    state.gpr[4] = handle;
    Reject([&] { dispatcher.Execute(state, 0x80203b4cu); },
           "not mapped to GameCube RAM");
    ppc_write_u32(state, handle + 0x1c, 0);
    for (uint32_t getter : {0x80199f5cu, 0x80199fb8u}) {
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = 2;
        dispatcher.Execute(state, getter);
        CheckReturn(state);
        Require(state.gpr[3] == 0, "Original game archive getter did not reject an empty resource.");
    }
}

void TestOriginalDiscArchive(const std::filesystem::path& directory,
                             const std::filesystem::path& rvz) {
    RvzImage disc(rvz);
    Require(disc.metadata().discId == "GYWEE9" && disc.metadata().discNumber == 0 &&
                disc.metadata().discVersion == 0,
            "Resource tests require the original US revision 0 disc.");
    const auto executables = ReadGameCubeExecutables(disc);
    VerifyUsExecutables(executables);
    Require(Sha1Hex(executables[1].bytes) == "dd220d152ea52a30a6142064ad4ab3aec64281e3",
            "Original resource test disc FST hash mismatch.");
    const auto file = ReadGameCubeFile(disc, "arowana.arc");
    Require(file.bytes.size() == 11808 &&
                Sha1Hex(file.bytes) == "69a6dcb01e1aa9cd797b809d0726ee559e239df0",
            "Original US resource archive hash mismatch.");
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    constexpr uint32_t archive = 0x80700000u;
    constexpr uint32_t handle = 0x80001000u;
    constexpr uint32_t nestedHandle = 0x80001200u;
    constexpr uint32_t path = 0x80001400u;
    const auto destination = ppc_memory_offset(state, archive, file.bytes.size());
    std::copy(file.bytes.begin(), file.bytes.end(), state.memory.begin() + destination);
    const auto before = state.memory;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsSceneDependencyTranslations());
    dispatcher.Register(1, state, UsArchiveTranslations());
    PrepareCall(state);
    state.gpr[3] = handle;
    state.gpr[4] = archive;
    dispatcher.Execute(state, 0x80199eecu);
    CheckReturn(state);
    Require(state.gpr[3] == 1 && ppc_read_u32(state, handle + 0xc) == 6,
            "Original SDK did not initialize the real disc archive.");
    struct ExpectedFile {
        const char* name;
        uint32_t offset;
        uint32_t size;
    };
    const std::array<ExpectedFile, 5> expected = {{
        {"arowana.act", 192, 444}, {"arowana.gpl", 640, 4500},
        {"arowana.tpl", 5152, 2816}, {"arowana.anm.arc", 7968, 1084},
        {"arowana.skn", 9056, 2752}}};
    for (uint32_t index = 1; index <= expected.size(); ++index) {
        const auto& entry = expected[index - 1];
        const std::string name(entry.name);
        for (size_t i = 0; i <= name.size(); ++i) {
            ppc_write_u8(state, path + static_cast<uint32_t>(i),
                         i == name.size() ? 0 : static_cast<uint8_t>(name[i]));
        }
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = path;
        dispatcher.Execute(state, 0x80203c4cu);
        CheckReturn(state);
        Require(state.gpr[3] == index, "Original SDK could not find a real disc archive entry.");
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = index;
        dispatcher.Execute(state, 0x80199f5cu);
        CheckReturn(state);
        Require(state.gpr[3] == archive + entry.offset,
                "Original game getter returned an incorrect real-resource pointer.");
        const auto payload = ppc_memory_offset(state, state.gpr[3], entry.size);
        Require(std::equal(file.bytes.begin() + entry.offset,
                           file.bytes.begin() + entry.offset + entry.size,
                           state.memory.begin() + payload),
                "Native game archive access changed the original disc payload.");
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = index;
        dispatcher.Execute(state, 0x80199fb8u);
        CheckReturn(state);
        Require(state.gpr[3] == entry.size,
                "Original game getter returned an incorrect real-resource length.");
    }
    PrepareCall(state);
    state.gpr[3] = nestedHandle;
    state.gpr[4] = archive + expected[3].offset;
    dispatcher.Execute(state, 0x80199eecu);
    CheckReturn(state);
    Require(state.gpr[3] == 1 && ppc_read_u32(state, nestedHandle) == archive + 7968 &&
                ppc_read_u32(state, nestedHandle + 8) == archive + 7968 + 0x40 &&
                ppc_read_u32(state, nestedHandle + 0xc) == 2,
            "Original SDK could not initialize the real nested animation archive.");
    CheckInitializationWrites(fixture, before, {
        {handle, 0x20}, {nestedHandle, 0x20}, {path, 64}});
}

void TestOriginalDvdPaths(const std::filesystem::path& directory) {
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsDvdTranslations());
    constexpr uint32_t fst = 0x80400000u;
    constexpr uint32_t path = 0x80001000u;
    constexpr uint32_t info = 0x80001400u;
    constexpr uint32_t buffer = 0x80001600u;
    constexpr uint32_t names = fst + 7 * 12;
    const auto node = [&](uint32_t index, uint32_t typeName, uint32_t offset, uint32_t size) {
        ppc_write_u32(state, fst + index * 12, typeName);
        ppc_write_u32(state, fst + index * 12 + 4, offset);
        ppc_write_u32(state, fst + index * 12 + 8, size);
    };
    node(0, 0x01000000, 0, 7);
    node(1, 0x01000001, 0, 5);
    node(2, 5, 0x100, 7);
    node(3, 15, 0x200, 0);
    node(4, 0x01000019, 1, 5);
    node(5, 31, 0x300, 64);
    node(6, 5, 0x400, 9);
    WriteGuestString(state, names, "");
    WriteGuestString(state, names + 1, "dir");
    WriteGuestString(state, names + 5, "child.bin");
    WriteGuestString(state, names + 15, "empty.bin");
    WriteGuestString(state, names + 25, "inner");
    WriteGuestString(state, names + 31, "longfilename.arc");
    ppc_write_u32(state, 0x80000038u, fst);
    ppc_write_u32(state, 0x803496e8u, 0);
    ppc_write_u32(state, 0x803496ecu, 1);
    const auto before = state.memory;
    PrepareCall(state);
    dispatcher.Execute(state, 0x801ba340u);
    CheckReturn(state);
    Require(ppc_read_u32(state, 0x803496d8u) == 0x80000000u &&
                ppc_read_u32(state, 0x803496dcu) == fst &&
                ppc_read_u32(state, 0x803496e0u) == names &&
                ppc_read_u32(state, 0x803496e4u) == 7,
            "Original DVD FST initialization differs from the disc/boot ABI.");
    for (const auto& lookup : std::vector<std::pair<std::string, uint32_t>>{
             {"", 0}, {"/", 0}, {".", 0}, {"..", 0}, {"DIR/CHILD.BIN", 2},
             {"/dir/./child.bin", 2}, {"dir/inner/../empty.bin", 3},
             {"dir/../child.bin", 6}, {"dir//child.bin", 6},
             {"//dir/child.bin", 2}, {"longfilename.arc", 5},
             {"child.bin", 6}, {"child.bin/extra", 0xffffffffu},
             {"dir/child", 0xffffffffu}, {"missing", 0xffffffffu},
             {"dir/empty.bin", 3}, {"dir/inner", 4}}) {
        WriteGuestString(state, path, lookup.first);
        PrepareCall(state);
        state.gpr[3] = path;
        dispatcher.Execute(state, 0x801ba378u);
        CheckReturn(state);
        Require(state.gpr[3] == lookup.second, "Original DVD path lookup semantics differ.");
    }
    for (const auto& change : std::vector<std::pair<std::string, uint32_t>>{
             {"dir", 1}, {"inner", 1}, {"..", 1}, {"child.bin", 0},
             {"missing", 0}, {"/", 1}}) {
        const uint32_t previous = ppc_read_u32(state, 0x803496e8u);
        WriteGuestString(state, path, change.first);
        PrepareCall(state);
        state.gpr[3] = path;
        dispatcher.Execute(state, 0x801ba97cu);
        CheckReturn(state);
        Require(state.gpr[3] == change.second &&
                    (change.second != 0 || ppc_read_u32(state, 0x803496e8u) == previous),
                "Original DVD directory change failed or changed state on rejection.");
    }
    WriteGuestString(state, path, "dir/inner");
    PrepareCall(state);
    state.gpr[3] = path;
    dispatcher.Execute(state, 0x801ba97cu);
    CheckReturn(state);
    for (uint32_t capacity : {1u, 5u, 64u}) {
        PrepareCall(state);
        state.gpr[3] = buffer;
        state.gpr[4] = capacity;
        dispatcher.Execute(state, 0x801ba8b8u);
        CheckReturn(state);
        const std::string expected = std::string("/dir/inner/").substr(0, capacity - 1);
        const size_t offset = ppc_memory_offset(state, buffer, capacity);
        Require(state.gpr[3] == (capacity == 64 ? 1u : 0u) &&
                    std::equal(expected.begin(), expected.end(), state.memory.begin() + offset) &&
                    ppc_read_u8(state, buffer + static_cast<uint32_t>(expected.size())) == 0,
                "Original DVD current directory or truncated output differs.");
    }
    ppc_write_u32(state, 0x803496e8u, 0);
    WriteGuestString(state, path, "dir/empty.bin");
    PrepareCall(state);
    state.gpr[3] = path;
    state.gpr[4] = info;
    dispatcher.Execute(state, 0x801ba66cu);
    CheckReturn(state);
    Require(state.gpr[3] == 1 && ppc_read_u32(state, info + 0x30) == 0x200 &&
                ppc_read_u32(state, info + 0x34) == 0 &&
                ppc_read_u32(state, info + 0x38) == 0 &&
                ppc_read_u32(state, info + 0xc) == 0,
            "Original DVDOpen did not initialize an empty file's DVDFileInfo.");
    WriteGuestString(state, path, "dir");
    const auto infoBefore = state.memory;
    PrepareCall(state);
    state.gpr[3] = path;
    state.gpr[4] = info;
    dispatcher.Execute(state, 0x801ba66cu);
    CheckReturn(state);
    const auto offset = ppc_memory_offset(state, info, 0x3c);
    Require(state.gpr[3] == 0 &&
                std::equal(infoBefore.begin() + offset, infoBefore.begin() + offset + 0x3c,
                           state.memory.begin() + offset),
            "Original DVDOpen accepted a directory or modified its rejected output.");
    WriteGuestString(state, path, "missing");
    PrepareCall(state);
    state.gpr[3] = path;
    state.gpr[4] = info;
    Reject([&] { dispatcher.Execute(state, 0x801ba66cu); },
           "No native translation registered at 0x801B346C");
    Require(std::equal(infoBefore.begin() + offset, infoBefore.begin() + offset + 0x3c,
                       state.memory.begin() + offset),
            "Original DVDOpen changed rejected output before its missing-file report.");
    CheckInitializationWrites(fixture, before, {
        {path, 128}, {info, 0x3c}, {buffer, 64}, {0x803496d8u, 0x14}});
    ppc_write_u32(state, 0x803496ecu, 0);
    WriteGuestString(state, path, "longfilename.arc");
    PrepareCall(state);
    state.gpr[3] = path;
    Reject([&] { dispatcher.Execute(state, 0x801ba378u); },
           "No native translation registered at 0x801770A8");
    Require(state.gpr[4] == 0x183, "Original DVD short-name validation missed its panic.");
    ppc_write_u32(state, 0x80000038u, 0);
    const auto beforeEmpty = state.memory;
    PrepareCall(state);
    dispatcher.Execute(state, 0x801ba340u);
    CheckReturn(state);
    Require(ppc_read_u32(state, 0x803496dcu) == 0 &&
                ppc_read_u32(state, 0x803496e0u) == names &&
                ppc_read_u32(state, 0x803496e4u) == 7,
            "Original null-FST initialization changed fields after its early return.");
    CheckInitializationWrites(fixture, beforeEmpty, {{0x803496d8u, 8}});
}

void TestOriginalDvdReadHelpers(const std::filesystem::path& directory) {
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsGameDvdReadTranslations());
    dispatcher.Register(1, state, UsInterruptServices());
    constexpr uint32_t info = 0x80001400u;
    constexpr uint32_t values = 0x80001800u;
    const auto before = state.memory;
    for (bool enabled : {false, true}) {
        for (uint32_t status : {0xffffffffu, 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u}) {
            state.externalInterruptsEnabled = enabled;
            ppc_write_u32(state, info + 0xc, status);
            PrepareCall(state);
            state.gpr[3] = info;
            dispatcher.Execute(state, 0x801bd074u);
            CheckReturn(state);
            Require(state.gpr[3] == (status == 3 ? 1u : status) &&
                        state.externalInterruptsEnabled == enabled,
                    "Original DVD status normalization or nested interrupt restoration differs.");
        }
    }
    state.externalInterruptsEnabled = true;
    ppc_write_u8(state, info + 0x54, 0);
    PrepareCall(state);
    state.gpr[3] = info;
    state.gpr[4] = 0xcc000000u;
    state.gpr[5] = 32;
    state.gpr[6] = 0;
    dispatcher.Execute(state, 0x8017b07cu);
    CheckReturn(state);
    Require(state.gpr[3] == 0 && ppc_read_u8(state, info + 0x55) == 0,
            "Original game DVD read did not reject a closed file before touching its destination.");
    for (uint32_t address : {0x80800000u, 0x80800001u, 0x8080001fu}) {
        for (uint32_t length : {64u, 65u, 95u}) {
            for (uint32_t offset : {0u, 1u, 3u, 4u}) {
                ppc_write_u32(state, values, address);
                ppc_write_u32(state, values + 4, length);
                ppc_write_u32(state, values + 8, offset);
                PrepareCall(state);
                state.gpr[3] = values;
                state.gpr[4] = values + 4;
                state.gpr[5] = values + 8;
                dispatcher.Execute(state, 0x8017b4ecu);
                CheckReturn(state);
                Require(ppc_read_u32(state, values) == ((address + 31) & ~31u) &&
                            ppc_read_u32(state, values + 4) ==
                                ((length - ((address & 31) != 0 ? 32u : 0u)) & ~31u) &&
                            ppc_read_u32(state, values + 8) == ((offset + 3) & ~3u),
                        "Original game DVD DMA alignment helper changed its rounding behavior.");
            }
        }
    }
    PrepareCall(state);
    state.gpr[3] = 0;
    state.gpr[4] = 0;
    state.gpr[5] = 0;
    dispatcher.Execute(state, 0x8017b4ecu);
    CheckReturn(state);
    CheckInitializationWrites(fixture, before, {{info + 0xc, 4}, {info + 0x55, 1}, {values, 12}});
}

void TestOriginalGameDvdRead(PpcState& state, NativeDispatcher& dispatcher,
                             NativeDvdQueue& queue, RvzImage& disc,
                             const UsModuleFixture& fixture, uint32_t info,
                             uint32_t destination, uint32_t length) {
    constexpr uint32_t observer = 0x80001a00u;
    ppc_write_u32(state, observer, 0x802c9ec0u);
    ppc_write_u8(state, observer + 4, 0);
    ppc_write_u32(state, info + 0x58, observer);
    ppc_write_u8(state, info + 0x55, 0);
    const auto before = state.memory;
    PrepareCall(state);
    state.gpr[3] = info;
    state.gpr[4] = destination;
    state.gpr[5] = length;
    state.gpr[6] = 0;
    dispatcher.Execute(state, 0x8017b07cu);
    CheckReturn(state);
    Require(state.gpr[3] == 1 && queue.pending() == 1 &&
                ppc_read_u32(state, info + 0x38) == 0x8017acf0u &&
                ppc_read_u32(state, info + 0x3c) == destination &&
                ppc_read_u32(state, info + 0x40) == length &&
                ppc_read_u32(state, info + 0x44) == 0 &&
                ppc_read_u8(state, observer + 4) == 0 &&
                ppc_read_u8(state, info + 0x55) == 0,
            "Original game DVD read did not submit asynchronously or changed its original return convention.");
    CheckInitializationWrites(fixture, before, {{info, 0x60}});
    state.externalInterruptsEnabled = false;
    Require(!queue.Pump() && queue.pending() == 1 && ppc_read_u8(state, observer + 4) == 0,
            "Cooperative DVD completion ignored the guest external interrupt mask.");
    state.externalInterruptsEnabled = true;
    Require(queue.Pump() && !queue.Pump(), "Original game DVD read did not complete exactly once.");
    CheckReturn(state);
    Require(ppc_read_u8(state, info + 0x55) == 1 &&
                ppc_read_u32(state, info + 0x58) == 0 &&
                ppc_read_u8(state, observer + 4) == 1 &&
                ppc_read_u32(state, info + 0xc) == 0 &&
                ppc_read_u32(state, info + 0x20) == length &&
                state.externalInterruptsEnabled,
            "Original game callback/virtual completion observer did not consume the read notification.");
    const auto bytes = disc.Read(ppc_read_u32(state, info + 0x30), length);
    const auto output = ppc_memory_offset(state, destination, length);
    Require(std::equal(bytes.begin(), bytes.end(), state.memory.begin() + output),
            "Original game DVD read changed payload bytes.");
    CheckInitializationWrites(fixture, before, {{info, 0x60}, {observer, 8}, {destination, length}});

    ppc_write_u8(state, observer + 4, 0);
    ppc_write_u32(state, info + 0x58, observer);
    PrepareCall(state);
    state.gpr[3] = info;
    state.gpr[4] = destination;
    state.gpr[5] = length;
    state.gpr[6] = 0;
    dispatcher.Execute(state, 0x8017b07cu);
    CheckReturn(state);
    const auto beforeCancel = state.memory;
    PrepareCall(state);
    state.gpr[3] = info;
    state.gpr[4] = 0;
    dispatcher.Execute(state, 0x801bd1ccu);
    CheckReturn(state);
    Require(state.gpr[3] == 1 && queue.pending() == 0 && !queue.Pump() &&
                ppc_read_u8(state, info + 0x55) == 0 &&
                ppc_read_u32(state, info + 0x58) == 0 &&
                ppc_read_u8(state, observer + 4) == 1 &&
                ppc_read_u32(state, info + 0xc) == 10,
            "Original game DVD cancellation did not notify its observer without setting success.");
    CheckInitializationWrites(fixture, beforeCancel, {{info, 0x60}, {observer, 8}});
}

void DvdResourceCallback(PpcState& state) {
    constexpr uint32_t log = 0x80001800u;
    const uint32_t count = ppc_read_u32(state, log);
    Require(count < 16, "DVD resource callback log overflow.");
    ppc_write_u32(state, log + 4 + count * 12, state.gpr[3]);
    ppc_write_u32(state, log + 8 + count * 12, state.gpr[4]);
    ppc_write_u32(state, log + 12 + count * 12, ppc_read_u32(state, state.gpr[4] + 0xc));
    ppc_write_u32(state, log, count + 1);
    state.pc = state.lr;
}

void TestOriginalDiscDvd(const std::filesystem::path& directory,
                         const std::filesystem::path& rvz) {
    RvzImage disc(rvz);
    const auto executables = ReadGameCubeExecutables(disc);
    VerifyUsExecutables(executables);
    Require(Sha1Hex(executables[1].bytes) == "dd220d152ea52a30a6142064ad4ab3aec64281e3",
            "Original DVD test FST hash mismatch.");
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsDvdTranslations());
    dispatcher.Register(1, state, UsClzTranslations());
    dispatcher.Register(2, state, UsArchiveTranslations());
    dispatcher.Register(3, state, UsSceneDependencyTranslations());
    dispatcher.Register(4, state, UsDvdAsyncTranslations());
    dispatcher.Register(5, state, UsDvdHostServices());
    dispatcher.Register(7, state, UsGameDvdReadTranslations());
    dispatcher.Register(8, state, UsInterruptServices());
    constexpr uint32_t fst = 0x80400000u;
    constexpr uint32_t path = 0x80001000u;
    constexpr uint32_t info = 0x80001400u;
    constexpr uint32_t archive = 0x80800000u;
    constexpr uint32_t compressed = 0x80500000u;
    constexpr uint32_t handle = 0x80001600u;
    constexpr uint32_t log = 0x80001800u;
    constexpr uint32_t callback = 0x80002400u;
    ppc_write_u32(state, callback, 0x4e800020);
    ppc_write_u32(state, log, 0);
    dispatcher.Register(6, state, {{callback, DvdResourceCallback, {0x4e800020}}});
    const auto offset = ppc_memory_offset(state, fst, executables[1].bytes.size());
    std::copy(executables[1].bytes.begin(), executables[1].bytes.end(),
              state.memory.begin() + offset);
    ppc_write_u32(state, 0x80000038u, fst);
    ppc_write_u32(state, 0x803496e8u, 0);
    ppc_write_u32(state, 0x803496ecu, 1);
    PrepareCall(state);
    dispatcher.Execute(state, 0x801ba340u);
    CheckReturn(state);
    const auto before = state.memory;
    const auto files = ReadGameCubeFileTable(disc);
    for (const auto& file : files) {
        Require(file.path.size() < 256, "Original DVD fixture path exceeds its buffer.");
        WriteGuestString(state, path, "/" + file.path);
        PrepareCall(state);
        state.gpr[3] = path;
        state.gpr[4] = info;
        dispatcher.Execute(state, 0x801ba66cu);
        CheckReturn(state);
        Require(state.gpr[3] == 1 && ppc_read_u32(state, info + 0x30) == file.offset &&
                    ppc_read_u32(state, info + 0x34) == file.size &&
                    ppc_read_u32(state, info + 0x38) == 0 &&
                    ppc_read_u32(state, info + 0xc) == 0,
                "Original DVDOpen disagrees with the validated real-disc FST.");
    }
    Require(files.size() == 1660, "Unexpected original US DVD file count.");
    CheckInitializationWrites(fixture, before, {{path, 257}, {info, 0x3c}});
    NativeDvdReader reader(disc);
    NativeDvdQueue queue(reader, dispatcher, state);
    state.submitDvdRead = [&](PpcState& guest) { queue.Submit(guest); };
    state.cancelDvdRead = [&](PpcState& guest) { queue.Cancel(guest); };
    for (const std::string name : {"arowana.arc", "roomcommon.arc.clz"}) {
        const auto file = ReadGameCubeFile(disc, name);
        WriteGuestString(state, path, "/" + name);
        // Construct a fresh original game file object, not a replacement host-side handle.
        const auto object = ppc_memory_offset(state, info, 0x60);
        std::fill(state.memory.begin() + object, state.memory.begin() + object + 0x60, 0);
        const auto beforeOpen = state.memory;
        PrepareCall(state);
        state.gpr[3] = info;
        state.gpr[4] = path;
        state.gpr[5] = 2;
        dispatcher.Execute(state, 0x8017ad8cu);
        CheckReturn(state);
        const uint32_t rounded = (static_cast<uint32_t>(file.bytes.size()) + 31) & ~31u;
        Require(state.gpr[3] == info && ppc_read_u8(state, info + 0x54) == 1 &&
                    ppc_read_u32(state, info + 0x50) == rounded &&
                    ppc_read_u32(state, info + 0x4c) == 2 &&
                    ppc_read_u32(state, info + 0x2c) == info &&
                    ppc_read_u32(state, info + 0x5c) == 0x802c9e74u,
                "Original game DvdFile construction/open ABI differs.");
        CheckInitializationWrites(fixture, beforeOpen, {{info, 0x60}});
        const auto beforeRead = state.memory;
        const uint32_t destination = name == "arowana.arc" ? archive : compressed;
        const uint32_t previousCallbacks = ppc_read_u32(state, log);
        PrepareCall(state);
        state.gpr[3] = info;
        state.gpr[4] = destination;
        state.gpr[5] = rounded;
        state.gpr[6] = 0;
        state.gpr[7] = callback;
        state.gpr[8] = 2;
        dispatcher.Execute(state, 0x801ba9dcu);
        CheckReturn(state);
        const auto target = ppc_memory_offset(state, destination, rounded);
        Require(state.gpr[3] == 1 && queue.pending() == 1 &&
                    ppc_read_u32(state, info + 0xc) == 2 &&
                    ppc_read_u32(state, info + 0x20) == 0 &&
                    ppc_read_u32(state, info + 0x28) == 0x801baa9cu &&
                    ppc_read_u32(state, info + 0x38) == callback &&
                    ppc_read_u32(state, log) == previousCallbacks &&
                    std::equal(beforeRead.begin() + target, beforeRead.begin() + target + rounded,
                               state.memory.begin() + target),
                "Original DVDReadAsyncPrio did not defer transfer/callback with SDK command ABI.");
        CheckInitializationWrites(fixture, beforeRead, {{info, 0x3c}});
        Require(queue.Pump() && !queue.Pump() && queue.pending() == 0,
                "Native DVD queue did not complete exactly one original SDK read.");
        CheckReturn(state);
        Require(ppc_read_u32(state, log) == previousCallbacks + 1 &&
                    ppc_read_u32(state, log + 4 + previousCallbacks * 12) == rounded &&
                    ppc_read_u32(state, log + 8 + previousCallbacks * 12) == info &&
                    ppc_read_u32(state, log + 12 + previousCallbacks * 12) == 0 &&
                    ppc_read_u32(state, info + 0x20) == rounded,
                "Original SDK callback forwarder lost the completion result, fileInfo or status.");
        const auto expected = disc.Read(ppc_read_u32(state, info + 0x30), rounded);
        Require(std::equal(expected.begin(), expected.end(), state.memory.begin() + target),
                "Native DVD read changed disc payload or synthesized EOF padding.");
        CheckInitializationWrites(fixture, beforeRead, {
            {destination, rounded}, {info, 0x3c}, {log, 4 + 16 * 12}});
        if (destination == compressed) {
            PrepareCall(state);
            state.gpr[3] = archive;
            state.gpr[4] = compressed;
            dispatcher.Execute(state, 0x8017837cu);
            CheckReturn(state);
            const auto decoded = DecodeClz(file.bytes, 0x800000);
            const auto output = ppc_memory_offset(state, archive, decoded.size());
            Require(state.gpr[3] == decoded.size() &&
                        std::equal(decoded.begin(), decoded.end(), state.memory.begin() + output),
                    "DVD-opened/read CLZ differs after original compiled decompression.");
        }
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = archive;
        dispatcher.Execute(state, 0x80199eecu);
        CheckReturn(state);
        Require(state.gpr[3] == 1, "DVD-opened/read resource failed original archive initialization.");
        PrepareCall(state);
        state.gpr[3] = info;
        state.gpr[4] = destination;
        state.gpr[5] = rounded;
        state.gpr[6] = 0;
        state.gpr[7] = callback;
        state.gpr[8] = 0;
        dispatcher.Execute(state, 0x801ba9dcu);
        CheckReturn(state);
        const auto beforeCancel = state.memory;
        const uint32_t previousCancel = ppc_read_u32(state, log);
        PrepareCall(state);
        state.gpr[3] = info;
        state.gpr[4] = callback;
        dispatcher.Execute(state, 0x801bd1ccu);
        CheckReturn(state);
        Require(state.gpr[3] == 1 && queue.pending() == 0 && !queue.Pump() &&
                    ppc_read_u32(state, info + 0xc) == 10 &&
                    ppc_read_u32(state, info + 0x20) == 0 &&
                    ppc_read_u32(state, log) == previousCancel + 2 &&
                    ppc_read_u32(state, log + 4 + previousCancel * 12) == 0xfffffffdu &&
                    ppc_read_u32(state, log + 16 + previousCancel * 12) == 0,
                "Native queued cancellation did not use the original SDK callback with -3 then cancel result 0.");
        CheckInitializationWrites(fixture, beforeCancel, {{info, 0x3c}, {log, 4 + 16 * 12}});
        TestOriginalGameDvdRead(state, dispatcher, queue, disc, fixture, info, destination, rounded);
    }
    state.submitDvdRead = {};
    state.cancelDvdRead = {};
    std::cout << "Original game async DVD read, callbacks, observer and cancellation verified; "
                 "DVDOpen verified all 1660 disc files; original game DvdFile -> "
                 "compiled DVDReadAsyncPrio -> native queued RVZ read -> original SDK callback -> "
                 "compiled CLZ/U8 initialization passed; queued cancellation delivered -3 then 0.\n";
}

void TestClzSemantics(const std::filesystem::path& directory) {
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsClzTranslations());
    constexpr uint32_t source = 0x80400000u;
    constexpr uint32_t destination = 0x80800000u;
    const auto run = [&](const std::vector<uint8_t>& bytes, uint32_t size) {
        const auto expected = DecodeClz(bytes, 8192);
        const auto input = ppc_memory_offset(state, source, bytes.size());
        std::copy(bytes.begin(), bytes.end(), state.memory.begin() + input);
        const auto before = state.memory;
        PrepareCall(state);
        state.gpr[3] = destination;
        state.gpr[4] = source;
        dispatcher.Execute(state, 0x8017837cu);
        CheckReturn(state);
        const auto output = ppc_memory_offset(state, destination, size);
        Require(state.gpr[3] == size && expected.size() == size &&
                    std::equal(expected.begin(), expected.end(), state.memory.begin() + output),
                "Original CLZ synthetic token/block semantics mismatch.");
        CheckInitializationWrites(fixture, before, {{destination, size}});
    };
    std::vector<uint8_t> bytes(12, 0);
    bytes[0] = 'C';
    bytes[1] = 'L';
    bytes[2] = 'Z';
    run(bytes, 0);
    bytes.resize(20);
    bytes[7] = 19;
    bytes[15] = 19;
    bytes[16] = 2;
    bytes[17] = 'x';
    bytes[18] = 0xff;
    bytes[19] = 0xff;
    run(bytes, 19);
    bytes.assign(71, 0);
    bytes[0] = 'C';
    bytes[1] = 'L';
    bytes[2] = 'Z';
    bytes[7] = 7;
    bytes[11] = 32;
    bytes[15] = 1;
    bytes[17] = 'z';
    for (size_t block : {32u, 64u}) {
        bytes[block + 3] = 3;
        bytes[block + 4] = 1;
        bytes[block + 5] = 0xff;
        bytes[block + 6] = 0xf0;
    }
    run(bytes, 7);
    bytes.assign(16, 0);
    bytes[0] = 'C';
    bytes[1] = 'L';
    bytes[2] = 'Z';
    bytes[6] = 0x10;
    bytes[7] = 3;
    bytes[14] = 0x10;
    bytes[15] = 3;
    for (size_t group = 0; group < 512; ++group) {
        bytes.push_back(0);
        for (size_t i = 0; i < 8; ++i) {
            bytes.push_back(static_cast<uint8_t>(group * 8 + i));
        }
    }
    bytes.insert(bytes.end(), {1, 0, 0});
    run(bytes, 4099);
    for (uint32_t invalid : {0u, 0x434c5a01u}) {
        ppc_write_u32(state, source, invalid);
        const auto before = state.memory;
        PrepareCall(state);
        state.gpr[3] = source;
        dispatcher.Execute(state, 0x801782f4u);
        CheckReturn(state);
        Require(state.gpr[3] == 0, "Original CLZ getter accepted invalid magic/version.");
        PrepareCall(state);
        state.gpr[3] = destination;
        state.gpr[4] = source;
        dispatcher.Execute(state, 0x8017837cu);
        CheckReturn(state);
        Require(state.gpr[3] == 0, "Original CLZ decoder accepted invalid magic/version.");
        CheckInitializationWrites(fixture, before);
    }
}

void TestOriginalClz(const std::filesystem::path& directory,
                     const std::filesystem::path& rvz) {
    RvzImage disc(rvz);
    auto fixture = LoadUsModuleFixture(directory);
    auto& state = fixture.state;
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, UsClzTranslations());
    dispatcher.Register(1, state, UsArchiveTranslations());
    dispatcher.Register(2, state, UsSceneDependencyTranslations());
    constexpr uint32_t source = 0x80360000u;
    constexpr uint32_t destination = 0x80800000u;
    constexpr uint32_t handle = 0x80001000u;
    size_t count = 0;
    for (const auto& entry : ReadGameCubeFileTable(disc)) {
        if (entry.path.size() < 4 ||
            entry.path.substr(entry.path.size() - 4) != ".clz") {
            continue;
        }
        const auto file = ReadGameCubeFile(disc, entry.path);
        const auto decoded = DecodeClz(file.bytes, 0x800000);
        Require(file.bytes.size() <= destination - source && !decoded.empty(),
                "Original CLZ fixture does not fit its isolated guest buffers.");
        const auto input = ppc_memory_offset(state, source, file.bytes.size());
        const auto output = ppc_memory_offset(state, destination, decoded.size());
        std::copy(file.bytes.begin(), file.bytes.end(), state.memory.begin() + input);
        std::fill(state.memory.begin() + output,
                  state.memory.begin() + output + decoded.size(), 0xa5);
        const auto before = state.memory;
        PrepareCall(state);
        state.gpr[3] = source;
        dispatcher.Execute(state, 0x801782f4u);
        CheckReturn(state);
        Require(state.gpr[3] == decoded.size(),
                "Original CLZ size getter differs from the checked decoder.");
        PrepareCall(state);
        state.gpr[3] = destination;
        state.gpr[4] = source;
        dispatcher.Execute(state, 0x8017837cu);
        CheckReturn(state);
        Require(state.gpr[3] == decoded.size() &&
                    std::equal(decoded.begin(), decoded.end(), state.memory.begin() + output),
                "Original compiled CLZ output differs from the checked decoder.");
        CheckInitializationWrites(fixture, before, {
            {destination, static_cast<uint32_t>(decoded.size())}});
        PrepareCall(state);
        state.gpr[3] = handle;
        state.gpr[4] = destination;
        dispatcher.Execute(state, 0x80199eecu);
        CheckReturn(state);
        Require(state.gpr[3] == 1 && ppc_read_u32(state, handle + 0xc) > 1,
                "Decompressed original CLZ is not readable by the original U8 API.");
        const uint32_t nodes = ppc_read_u32(state, handle + 4);
        const uint32_t nodeCount = ppc_read_u32(state, handle + 0xc);
        Require(nodeCount <= decoded.size() / 12 &&
                    nodes >= destination &&
                    nodes - destination <= decoded.size() - nodeCount * 12,
                "Decoded U8 node table is outside the checked resource.");
        for (uint32_t index = 1; index < nodeCount; ++index) {
            const uint32_t node = nodes + index * 12;
            if (ppc_read_u8(state, node) != 0) {
                continue;
            }
            const uint32_t offset = ppc_read_u32(state, node + 4);
            const uint32_t length = ppc_read_u32(state, node + 8);
            Require(offset <= decoded.size() && length <= decoded.size() - offset,
                    "Decoded U8 payload is outside the checked resource.");
            PrepareCall(state);
            state.gpr[3] = handle;
            state.gpr[4] = index;
            dispatcher.Execute(state, 0x80199f5cu);
            CheckReturn(state);
            Require(state.gpr[3] == destination + offset,
                    "Original game getter returned a wrong decompressed payload pointer.");
            PrepareCall(state);
            state.gpr[3] = handle;
            state.gpr[4] = index;
            dispatcher.Execute(state, 0x80199fb8u);
            CheckReturn(state);
            Require(state.gpr[3] == length,
                    "Original game getter returned a wrong decompressed payload length.");
        }
        CheckInitializationWrites(fixture, before, {
            {destination, static_cast<uint32_t>(decoded.size())}, {handle, 0x20}});
        std::cout << entry.path << ": compiled CLZ verified, " << decoded.size()
                  << " bytes, SHA-1 " << Sha1Hex(decoded) << '\n';
        ++count;
    }
    Require(count == 11, "Unexpected original US compressed resource count.");
}

void TestSceneConstruction(const std::filesystem::path& directory, bool withDependencies) {
    for (uint32_t mode : {0u, 8u, 9u}) {
        auto fixture = LoadUsModuleFixture(directory);
        auto& state = fixture.state;
        NativeDispatcher dispatcher;
        dispatcher.Register(0, state, UsRelDolTranslations());
        dispatcher.Register(100, state, UsAllocatorTranslations());
        dispatcher.Register(101, state, UsSceneDolTranslations());
        if (withDependencies) {
            dispatcher.Register(102, state, UsSceneDependencyTranslations());
        }
        auto moduleBindings = UsRelTranslations();
        const auto scenes = UsSceneTranslations();
        moduleBindings.insert(moduleBindings.end(), scenes.begin(), scenes.end());
        if (withDependencies) {
            const auto dependencies = UsSceneDependencyRelTranslations();
            moduleBindings.insert(moduleBindings.end(), dependencies.begin(), dependencies.end());
        }
        dispatcher.RegisterRel(fixture.images[0], fixture.modules[0], state, moduleBindings);
        InitializeOriginalHeap(state, dispatcher);
        state.readTimeBase = MakeNativeTimeBase();
        constexpr uint32_t system = 0x80001000u;
        constexpr uint32_t context = 0x80002000u;
        constexpr uint32_t manager = 0x80002400u;
        constexpr uint32_t calendar = 0x80002800u;
        ppc_write_u32(state, 0x80349208u, system);
        ppc_write_u32(state, system + 0x5c, mode);
        ppc_write_u32(state, system + 0x4c, 0xdeadbeefu);
        ppc_write_u32(state, 0x80349194u, context);
        ppc_write_u32(state, context + 0x540, 0x16);
        ppc_write_u32(state, 0x8034923cu, manager);
        ppc_write_u32(state, 0x80349218u, calendar);
        ppc_write_u32(state, calendar, 123456789u);
        if (withDependencies) {
            PrepareCall(state);
            state.gpr[3] = manager;
            dispatcher.Execute(state, 0x8000b4c4u);
            CheckReturn(state);
            Require(state.gpr[3] == manager &&
                        ppc_read_u32(state, 0x8034923cu) == manager,
                    "Original manager constructor failed to publish its singleton.");
        }
        ppc_write_u32(state, 0x803491ccu, 0x80700000u);
        ppc_write_u8(state, 0x807299a4u, 7);
        const auto arena = ppc_memory_offset(state, kArenaBottom, kArenaSize);
        std::fill(state.memory.begin() + arena,
                  state.memory.begin() + arena + kArenaSize, 0xa5);
        const auto beforeBase = state.memory;
        PrepareCall(state);
        state.gpr[3] = kArenaBottom;
        state.gpr[4] = mode == 0 ? 7u : mode;
        dispatcher.Execute(state, 0x80012800u);
        CheckReturn(state);
        Require(state.gpr[3] == kArenaBottom &&
                    ppc_read_u32(state, kArenaBottom) == (mode == 0 ? 7u : mode) &&
                    ppc_read_u32(state, kArenaBottom + 4) == 0 &&
                    ppc_read_u32(state, kArenaBottom + 20) == 0x80298124u &&
                    std::equal(beforeBase.begin(), beforeBase.begin() + arena,
                               state.memory.begin()) &&
                    std::equal(beforeBase.begin() + arena + 24, beforeBase.end(),
                               state.memory.begin() + arena + 24),
                "Original scene base constructor changed its ABI or wrote beyond its object.");
        CheckCopy(state, kArenaBottom + 8, 0x8029810cu, 12);
        std::fill(state.memory.begin() + arena, state.memory.begin() + arena + 24, 0xa5);
        const auto beforeScene = state.memory;

        PrepareCall(state);
        const uint32_t target = withDependencies ?
            (mode == 0 ? 0x8001bd04u : mode == 8 ? 0x8100dcd8u : 0x8002283cu) :
            (mode == 0 ? 0x8000fee8u : mode == 8 ? 0x8017cd5cu : 0x8101ddb0u);
        std::ostringstream missing;
        missing << "No native translation registered at 0x" << std::hex
                << std::uppercase << target;
        Reject([&] { dispatcher.Execute(state, fixture.modules[0].prolog); }, missing.str());
        const uint32_t scene = kArenaBottom + kArenaSize - (mode == 9 ? 0xc0u : 0x60u);
        Require(state.pc == target && ppc_read_u32(state, system + 0x4c) == 0xdeadbeefu &&
                    ppc_read_u32(state, scene) == (mode == 0 ? 7u : mode) &&
                    ppc_read_u32(state, scene + 4) == 0,
                "Original scene base initialization or explicit dependency boundary mismatch.");
        CheckCopy(state, scene + 8, 0x8029810cu, 12);
        Require(ppc_read_u32(state, scene + 20) ==
                    (mode == 0 ? 0x81031620u : mode == 8 ? 0x81031a44u : 0x81031d78u) &&
                    ppc_read_u32(state, scene + 24) == (mode == 0 ? 10u : mode == 8 ? 3u : 8u),
                "Original scene-specific vtable or state initialization mismatch.");
        if (mode == 0) {
            Require(ppc_read_u32(state, scene + 0x2c) == 0 &&
                        state.gpr[3] == (withDependencies ? 2u : calendar) &&
                        state.gpr[27] == (123456789u / 8640000u) % 4 &&
                        state.gpr[28] == (123456789u / 864000u) % 10 &&
                        state.gpr[29] == 7 &&
                        ppc_read_u32(state, 0x80349494u) == 1,
                    "Original default-scene calendar fields or allocation count mismatch.");
            if (withDependencies) {
                Require(state.gpr[4] == (123456789u / 8640000u) % 4 &&
                            state.gpr[5] == (123456789u / 864000u) % 10 &&
                            state.gpr[6] == 7,
                        "Original calendar chain did not supply the next routine's arguments.");
            }
        } else if (mode == 8) {
            const uint32_t nested = scene - 0x60;
            Require(ppc_read_u32(state, scene + 0x24) == 0x16 &&
                        ppc_read_u32(state, scene + 0x28) == 0 &&
                        ppc_read_u32(state, scene + 0x54) == nested &&
                        state.gpr[3] == (withDependencies ? nested - 0x240u : manager) &&
                        state.gpr[4] == (withDependencies ? 1u : nested) &&
                        state.gpr[5] == (withDependencies ? 0u : 0xa000u) &&
                        ppc_read_u32(state, 0x80349494u) == (withDependencies ? 3u : 2u),
                    "Mode-8 nested scene allocation or registration arguments mismatch.");
            for (const auto offset : {0u, 0x18u, 0x30u}) {
                Require(ppc_read_u32(state, nested + offset) ==
                            (withDependencies && offset == 0 ? manager : 0) &&
                            ppc_read_u32(state, nested + offset + 4) ==
                                (withDependencies && offset == 0 ? manager + 0x14 : 0) &&
                            ppc_read_u32(state, nested + offset + 8) ==
                                (withDependencies && offset == 0 ? manager : 0) &&
                            ppc_read_u32(state, nested + offset + 12) == nested + offset,
                        "Mode-8 nested object's intrusive-list links mismatch.");
            }
            for (const auto offset : {0x10u, 0x28u, 0x40u, 0x48u}) {
                const uint32_t vtableOffset = offset == 0x10 ? 0u :
                    offset == 0x28 ? 0x1cu : offset == 0x40 ? 0x30u : 0x44u;
                Require(ppc_read_u32(state, nested + offset) == 0x81031a5cu + vtableOffset,
                        "Mode-8 nested object's adjusted vtable mismatch.");
            }
            if (withDependencies) {
                Require(ppc_read_u32(state, nested + 0x14) == 0xa000 &&
                            ppc_read_u32(state, manager + 0x14) == nested &&
                            ppc_read_u32(state, manager + 4) == nested &&
                            ppc_read_u32(state, manager + 0x18) == 0,
                        "Original mode-8 registration did not update manager sentinel links.");
            }
        } else {
            const uint32_t nested = scene - 0x40;
            const uint32_t nextObject = nested - 0x120;
            Require(ppc_read_u32(state, scene + 0x1c) == 0 &&
                        ppc_read_u32(state, scene + 0x20) == 0x16 &&
                        state.gpr[3] == (withDependencies ? nextObject - 0x4e0u : nextObject) &&
                        ppc_read_u32(state, 0x80349494u) == (withDependencies ? 4u : 3u) &&
                        ppc_read_u32(state, 0x80349190u) == nested,
                    "Mode-9 nested allocation or singleton construction mismatch.");
            for (uint32_t offset = 0x48; offset < 0xb4; ++offset) {
                Require(ppc_read_u8(state, scene + offset) == 0,
                        "Mode-9 scene buffers were not cleared by original memset.");
            }
            Require(ppc_read_u32(state, nested) == 0 &&
                        ppc_read_u32(state, nested + 4) == nested + 0x14 &&
                        ppc_read_u32(state, nested + 8) == nested &&
                        ppc_read_u32(state, nested + 0x14) == nested &&
                        ppc_read_u32(state, nested + 0x18) == 0 &&
                        ppc_read_u32(state, nested + 0x1c) == nested &&
                        ppc_read_u32(state, nested + 0x28) == 0x8029bdb0u,
                    "Original nested DOL object's list construction mismatch.");
            if (withDependencies) {
                Require(ppc_read_u32(state, nextObject + 0x10) == 0x81035d04u &&
                            ppc_read_u32(state, nextObject + 0x6c) == 0x802cb224u &&
                            ppc_read_u32(state, nextObject + 0x58) == 0 &&
                            ppc_read_u32(state, nextObject + 0x68) == 0 &&
                            ppc_read_u8(state, nextObject + 0x100) == 1,
                        "Original mode-9 nested vtables, empty archive or enabled flag mismatch.");
                for (uint32_t offset : {0x9cu, 0xc8u}) {
                    Require(ppc_read_u32(state, nextObject + offset) == 0 &&
                                ppc_read_u32(state, nextObject + offset + 4) == nextObject + offset + 0x14 &&
                                ppc_read_u32(state, nextObject + offset + 8) == nextObject + offset &&
                                ppc_read_u32(state, nextObject + offset + 0x14) == nextObject + offset &&
                                ppc_read_u32(state, nextObject + offset + 0x1c) == nextObject + offset &&
                                ppc_read_u32(state, nextObject + offset + 0x28) == 0x81035d58u,
                            "Original REL nested self-list construction mismatch.");
                }
            }
        }
        Require(ppc_read_u32(state, kArenaBottom - 4) == 0 &&
                    ppc_read_u32(state, kArenaBottom + kArenaSize) == 0 &&
                    ppc_read_u32(state, scene + (mode == 9 ? 0xb4u : mode == 8 ? 0x58u : 0x40u)) ==
                        0xa5a5a5a5u,
                "Scene construction crossed arena guards or initialized untouched padding.");
        CheckConstructorData(fixture);
        CheckInitializationWrites(fixture, beforeScene, {
            {kArenaBottom, kArenaSize},
            {kNodes, (ppc_read_u32(state, 0x80349494u) + 1) * 24}, {kHeap, 0x20},
            {0x80349190u, 4}, {0x80349494u, 8}, {manager, withDependencies ? 0xa0u : 0u}});
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 && !(argc == 4 && std::string(argv[2]) == "--rvz")) {
            throw std::runtime_error("Expected verified US executable directory [--rvz <US-disc.rvz>].");
        }
        TestOriginalFunctions(argv[1]);
        TestOriginalInitialization(argv[1]);
        TestOriginalAllocator(argv[1]);
        TestSceneServices(argv[1]);
        TestOriginalArchives(argv[1]);
        TestOriginalDvdPaths(argv[1]);
        TestOriginalDvdReadHelpers(argv[1]);
        TestClzSemantics(argv[1]);
        std::cout << "Original compiled CLZ header, literals, overlapping/distant references "
                     "and padded multi-block semantics passed.\n";
        if (argc == 4) {
            TestOriginalDiscArchive(argv[1], argv[3]);
            TestOriginalDiscDvd(argv[1], argv[3]);
            TestOriginalClz(argv[1], argv[3]);
            std::cout << "Original disc RVZ -> FST file read -> compiled U8/game getters passed; "
                         "all five resource payloads and nested animation archive verified.\n";
        }
        TestSceneConstruction(argv[1], false);
        TestSceneConstruction(argv[1], true);
        std::cout << "Two original relocated US REL functions executed as compiled native C++; "
                     "module binding, returns, invalidation and placement checks passed.\n"
                     "All 13 original wlp0 static constructors and its epilog passed; "
                     "without allocator bindings the original dependency failure is retained.\n"
                     "Original heap initialization/new/delete, split/exact/zero allocations, "
                     "coalescing/reuse and OOM path passed; "
                     "with allocator/time-base bindings the prolog reaches all three scene constructors.\n"
                     "Scene prefixes execute base/nested object construction, buffer initialization and "
                     "calendar getters, then stop explicitly at their next untranslated dependencies.\n"
                     "Original manager construction, sorted registration/reparenting and calendar boundaries passed; "
                     "extended scene paths reach their next game-object dependencies.\n"
                     "Original U8 archive initialization, path lookup, indexed open and game file "
                     "pointer/length wrappers passed with synthetic payloads; invalid magic reaches OSPanic.\n"
                     "Module initialization remains incomplete; this is not a game boot.\n";
    } catch (const std::exception& error) {
        std::cerr << "Original native REL test failed: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
