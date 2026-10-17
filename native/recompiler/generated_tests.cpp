#include "generated_ppc_tests.h"
#include "native_dispatch.h"
#include "native_time_base.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void TestLogicalRegisters() {
    for (uint32_t source : {0u, 1u, 0x80000000u, 0xffffffffu, 0x12345678u}) {
        for (uint32_t right : {0u, 1u, 31u, 32u, 63u, 64u, 0xffffffffu}) {
            for (unsigned operation = 0; operation < 4; ++operation) {
                PpcState state;
                state.gpr[3] = source;
                state.gpr[4] = 0xdeadbeefu;
                state.gpr[5] = right;
                state.xer = 0x80000000u;
                state.cr = 0x01234567u;
                const uint32_t shift = right & 63u;
                uint32_t expected = 0;
                switch (operation) {
                case 0:
                    ppc_00001060(state);
                    expected = shift >= 32 ? 0u : source << shift;
                    break;
                case 1:
                    ppc_00001070(state);
                    expected = source & right;
                    break;
                case 2:
                    ppc_000016F0(state);
                    expected = source | right;
                    break;
                case 3:
                    ppc_00001860(state);
                    expected = shift >= 32 ? 0u : source >> shift;
                    break;
                }
                Require(state.gpr[4] == expected && state.gpr[3] == source &&
                            state.gpr[5] == right,
                        "Compiled logical instruction used incorrect register fields or shift width.");
                const uint32_t cr = expected == 0 ? 3u :
                                    (expected & 0x80000000u) != 0 ? 9u : 5u;
                Require((state.cr >> 28) == cr && (state.cr & 0x0fffffffu) == 0x01234567u,
                        "Compiled logical record instruction produced incorrect CR0.");
            }
        }
    }
}

void TestCarryAndSpecialRegisters() {
    for (uint32_t source : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (bool subtract : {false, true}) {
            PpcState state;
            state.gpr[4] = source;
            state.xer = 0xc0000000u | (subtract ? 0u : 0x20000000u);
            const uint64_t sum = static_cast<uint64_t>(source) +
                                 (subtract ? 0xffffffffu : 1u);
            if (subtract) {
                ppc_00003010(state);
            } else {
                ppc_00003000(state);
            }
            Require(state.gpr[3] == static_cast<uint32_t>(sum) &&
                        ((state.xer >> 29) & 1u) == (sum >> 32) &&
                        (state.xer & 0xc0000000u) == 0xc0000000u,
                    "Compiled addic. produced incorrect carry or changed overflow flags.");
            const uint32_t result = static_cast<uint32_t>(sum);
            Require((state.cr >> 28) ==
                        (result == 0 ? 3u : result < 0x80000000u ? 5u : 9u),
                    "Compiled addic. produced incorrect comparison flags.");
        }
    }
    PpcState state;
    state.gpr[3] = 0xf1234567u;
    ppc_00003020(state);
    Require(state.ctr == state.gpr[3] && state.gpr[4] == state.ctr &&
                state.cr == 0, "CTR register transfer failed.");
    ppc_00003030(state);
    Require(state.xer == state.gpr[3] && state.gpr[4] == state.xer &&
                state.cr == 0, "XER register transfer failed.");
}

void TestLocalIndirectBranches() {
    for (uint32_t target : {0x4088u, 0x4090u, 0x4093u, 0x5003u}) {
        PpcState state;
        state.ctr = target;
        state.lr = 0x6003u;
        ppc_00004080(state);
        Require(state.gpr[3] == (target == 0x4088 ? 99u : target < 0x5000 ? 7u : 1u) &&
                    state.pc == (target < 0x5000 ? 0x6000u : 0x5000u) &&
                    state.ctr == target && state.lr == 0x6003u,
                "Local CTR jump table, alignment or external tail dispatch failed.");
    }
    for (bool taken : {false, true}) {
        for (uint32_t target : {0x40acu, 0x5000u}) {
            PpcState state;
            state.ctr = target;
            state.lr = 0x6000u;
            state.cr = taken ? 0x20000000u : 0;
            ppc_000040A0(state);
            Require(state.gpr[3] == (!taken ? 3u : target == 0x40ac ? 9u : 0u) &&
                        state.pc == (taken && target == 0x5000 ? 0x5000u : 0x6000u) &&
                        state.ctr == target && state.cr == (taken ? 0x20000000u : 0u),
                    "Conditional local/external CTR branch changed condition/fallthrough behavior.");
        }
    }
    PpcState state;
    state.ctr = 0x40e0u;
    state.lr = 0x6000u;
    ppc_000040E0(state);
    Require(state.gpr[3] == 3 && state.pc == 0x6000u && state.ctr == 0x40e0u,
            "Backward local CTR branch did not retain native loop semantics.");
}

void TestLinkedBranchAndMultiLoad() {
    for (bool taken : {false, true}) {
        PpcState state;
        state.cr = taken ? 0x20000000u : 0;
        state.gpr[3] = 7;
        unsigned calls = 0;
        state.callGuest = [&calls](uint32_t address, PpcState& guest) {
            Require(address == 0x3050 && guest.lr == 0x3044,
                    "Conditional link branch used incorrect target/LR.");
            ++calls;
            guest.gpr[3] = 41;
        };
        ppc_00003040(state);
        Require(calls == static_cast<unsigned>(taken) &&
                    state.gpr[3] == (taken ? 42u : 8u) &&
                    state.pc == 0x3048 && state.lr == 0x3044,
                "Conditional call did not return to its continuation.");
    }
    PpcState state;
    state.memory.resize(128);
    for (uint32_t reg = 3; reg < 32; ++reg) {
        ppc_write_u32(state, (reg - 3) * 4, 0x1000u + reg);
    }
    state.gpr[4] = 0;
    ppc_00003050(state);
    for (uint32_t reg = 3; reg < 32; ++reg) {
        Require(state.gpr[reg] == 0x1000u + reg,
                "lmw changed its effective address while loading its base register.");
    }
}

void TestComparisonAndNor() {
    for (uint32_t left : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (uint32_t right : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
            PpcState state;
            state.gpr[3] = left;
            state.gpr[4] = right;
            state.xer = 0x80000000u;
            state.cr = 0x01234567u;
            ppc_00003070(state);
            const int64_t signedLeft = left < 0x80000000u ? int64_t{left}
                                                         : int64_t{left} - 0x100000000ll;
            const int64_t signedRight = right < 0x80000000u ? int64_t{right}
                                                           : int64_t{right} - 0x100000000ll;
            const uint32_t signedField = signedLeft < signedRight ? 9u :
                                         signedLeft > signedRight ? 5u : 3u;
            Require(state.cr == ((signedField << 28) | 0x01234567u),
                    "cmpw did not compare signed registers or preserve other CR fields.");
            state.cr = 0x12345678u;
            ppc_00003080(state);
            const uint32_t unsignedField = left < right ? 9u : left > right ? 5u : 3u;
            Require(state.cr == ((0x12345678u & ~0x000f0000u) | (unsignedField << 16)),
                    "cmplw did not compare unsigned registers in the selected CR field.");
            state.gpr[5] = right;
            ppc_00003060(state);
            const uint32_t expected = ~(left | right);
            Require(state.gpr[4] == expected && state.gpr[3] == left &&
                        state.gpr[5] == right &&
                        (state.cr >> 28) ==
                            (expected == 0 ? 3u : expected < 0x80000000u ? 5u : 9u),
                    "nor. produced an incorrect result, register write or CR0.");
        }
    }
}

template <typename Function>
void Reject(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(message) != std::string::npos,
                "Unexpected native-dispatch rejection.");
        return;
    }
    throw std::runtime_error("Expected native-dispatch failure: " + message);
}

void TestCountLeadingZeros() {
    for (uint32_t bit = 0; bit <= 32; ++bit) {
        PpcState state;
        state.gpr[3] = bit == 32 ? 0u : 0x80000000u >> bit;
        state.gpr[4] = 0xdeadbeefu;
        state.cr = 0x01234567u;
        state.xer = 0xa0000000u;
        ppc_00003090(state);
        Require(state.gpr[4] == bit && state.xer == 0xa0000000u &&
                    state.cr == ((bit == 0 ? 3u : 5u) << 28 | 0x01234567u),
                "cntlzw. count, destination, CR0 or XER is incorrect.");
        state.cr = 0x12345678u;
        ppc_000030A0(state);
        Require(state.gpr[3] == bit && state.cr == 0x12345678u,
                "Non-record cntlzw changed CR or failed with identical source/destination.");
    }
}

void TestSignExtendHalfword() {
    for (uint32_t value : {0u, 1u, 0x7fffu, 0x8000u, 0xffffu,
                           0x80000000u, 0xffff7fffu, 0x12348001u}) {
        PpcState state;
        state.gpr[3] = value;
        state.gpr[4] = 0xdeadbeefu;
        state.xer = 0xe0000000u;
        state.cr = 0x01234567u;
        const uint32_t low = value & 0xffffu;
        const uint32_t expected = low >= 0x8000u ? 0xffff0000u | low : low;
        ppc_000030B0(state);
        Require(state.gpr[4] == expected && state.gpr[3] == value &&
                    state.xer == 0xe0000000u &&
                    state.cr == (((expected == 0 ? 3u :
                                  expected < 0x80000000u ? 5u : 9u) << 28) | 0x01234567u),
                "extsh. result, registers, CR0 or XER mismatch.");
        state.cr = 0x87654321u;
        ppc_000030C0(state);
        Require(state.gpr[3] == expected && state.cr == 0x87654321u,
                "Non-record extsh changed CR or failed with identical source/destination.");
    }
}

void TestSignExtendByteAndNegate() {
    for (uint32_t byte = 0; byte < 256; ++byte) {
        PpcState state;
        const uint32_t value = 0x12345600u | byte;
        const uint32_t expected = byte < 128 ? byte : 0xffffff00u | byte;
        state.gpr[3] = value;
        state.cr = 0x01234567u;
        state.xer = 0xe0000000u;
        ppc_00003180(state);
        Require(state.gpr[4] == expected && state.gpr[3] == value &&
                    state.xer == 0xe0000000u &&
                    state.cr == (((byte == 0 ? 3u : byte < 128 ? 5u : 9u) << 28) |
                                 0x01234567u),
                "Compiled extsb. result, CR0 or preserved registers mismatch.");
        const auto cr = state.cr;
        ppc_00003190(state);
        Require(state.gpr[3] == expected && state.cr == cr,
                "Non-record aliased extsb changed CR or used the wrong source.");
    }
    for (uint32_t value : {0u, 1u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        PpcState state;
        state.gpr[4] = value;
        state.cr = 0x01234567u;
        state.xer = 0xe0000000u;
        const uint32_t expected = 0u - value;
        ppc_000031A0(state);
        Require(state.gpr[3] == expected && state.gpr[4] == value &&
                    state.xer == 0xe0000000u &&
                    state.cr == (((expected == 0 ? 3u : expected < 0x80000000u ? 5u : 9u) << 28) |
                                 0x01234567u),
                "Compiled neg. result, CR0 or preserved registers mismatch.");
        const auto cr = state.cr;
        ppc_000031B0(state);
        Require(state.gpr[4] == expected && state.cr == cr && state.xer == 0xe0000000u,
                "Non-record aliased neg changed CR/XER or used the wrong source.");
    }
}

void TestTimeBaseAndConditionXor() {
    PpcState state;
    Reject([&] { ppc_000030D0(state); }, "No native PowerPC time-base source");
    unsigned reads = 0;
    state.cr = 0x87654321u;
    state.xer = 0xe0000000u;
    for (uint64_t ticks : {0ull, 0xffffffffull, 0x100000000ull,
                            0x12345678abcdef01ull, UINT64_MAX}) {
        state.readTimeBase = [&reads, ticks] { ++reads; return ticks; };
        ppc_000030D0(state);
        ppc_000030E0(state);
        Require(state.gpr[3] == static_cast<uint32_t>(ticks) &&
                    state.gpr[4] == static_cast<uint32_t>(ticks >> 32) &&
                    state.cr == 0x87654321u && state.xer == 0xe0000000u,
                "mftb low/high reads, rollover or CR/XER preservation failed.");
    }
    Require(reads == 10, "mftb did not perform exactly one native clock read per instruction.");
    for (uint32_t cr : {0u, 0xffffffffu, 0x01000000u, 0x00800000u, 0x87654321u}) {
        state.cr = cr;
        ppc_000030F0(state);
        const auto expected = (cr & ~(1u << 25)) |
                              ((((cr >> 24) ^ (cr >> 23)) & 1u) << 25);
        Require(state.cr == expected, "crxor changed the wrong condition bit.");
        ppc_00003100(state);
        Require(state.cr == (expected & ~(1u << 25)), "crclr did not clear exactly CR bit 6.");
    }
    Require(GameCubeTicksFromNanoseconds(0) == 0 &&
                GameCubeTicksFromNanoseconds(1000000000) == 40500000 &&
                GameCubeTicksFromNanoseconds(1500000000) == 60750000 &&
                GameCubeTicksFromNanoseconds(100000000001ll) == 4050000000ull &&
                GameCubeTicksFromNanoseconds(1000000000000ll) == 40500000000ull &&
                GameCubeTicksFromNanoseconds(INT64_MAX) == 373546567492618420ull,
            "Native time-base conversion has incorrect rate, truncation or overflow.");
    Reject([&] { GameCubeTicksFromNanoseconds(-1); }, "backwards");
    state.readTimeBase = MakeNativeTimeBase();
    const auto first = ppc_read_time_base(state);
    uint64_t last = first;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (last == first && std::chrono::steady_clock::now() < deadline) {
        last = ppc_read_time_base(state);
    }
    Require(last > first, "Native monotonic clock did not advance.");
}

void TestSubtractImmediateCarry() {
    for (uint32_t source : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (uint32_t immediate : {0u, 1u, 0xffffffffu}) {
            PpcState state;
            state.gpr[4] = source;
            state.cr = 0x87654321u;
            state.xer = 0xc0000000u | (immediate == 0 ? 0x20000000u : 0u);
            if (immediate == 0) {
                ppc_00003130(state);
            } else if (immediate == 1) {
                ppc_00003110(state);
            } else {
                ppc_00003120(state);
            }
            Require(state.gpr[3] == immediate - source && state.gpr[4] == source &&
                        state.cr == 0x87654321u &&
                        state.xer == (0xc0000000u |
                                      (immediate >= source ? 0x20000000u : 0u)),
                    "subfic result, carry, input register or CR/overflow preservation failed.");
        }
    }
}

void TestSubtractExtended() {
    for (uint32_t left : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu}) {
        for (uint32_t right : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xfffffffeu, 0xffffffffu}) {
            for (uint32_t carry : {0u, 1u}) {
                for (uint32_t flags : {0u, 0xc0000001u}) {
                    for (unsigned variant = 0; variant < 4; ++variant) {
                        PpcState state;
                        state.gpr[4] = left;
                        state.gpr[5] = right;
                        state.xer = flags | (carry << 29);
                        state.cr = 0x12345678u;
                        const uint32_t result = right - left - (1u - carry);
                        const bool noBorrow = uint64_t{right} >= uint64_t{left} + 1 - carry;
                        if (variant == 0) ppc_000031C0(state);
                        if (variant == 1) ppc_000031D0(state);
                        if (variant == 2) ppc_000031E0(state);
                        if (variant == 3) ppc_000031F0(state);
                        const uint32_t destination = variant == 2 ? 4u : variant == 3 ? 5u : 3u;
                        const bool record = variant == 1 || variant == 2;
                        const uint32_t comparison = result == 0 ? 2u :
                                                    result < 0x80000000u ? 4u : 8u;
                        const uint32_t cr = ((comparison | (flags >> 31)) << 28) | 0x02345678u;
                        Require(state.gpr[destination] == result &&
                                    (destination == 4 || state.gpr[4] == left) &&
                                    (destination == 5 || state.gpr[5] == right) &&
                                    state.xer == (flags | (noBorrow ? 0x20000000u : 0u)) &&
                                    state.cr == (record ? cr : 0x12345678u),
                                "Compiled subfe result, carry/borrow, CR0, flags or aliasing mismatch.");
                    }
                }
            }
        }
    }
}

void TestMultiplyDivideWords() {
    for (uint32_t left : {0u, 1u, 2u, 0x7fffffffu, 0x80000000u, 0xffffffffu}) {
        for (uint32_t right : {0u, 1u, 2u, 31u, 0x80000000u, 0xffffffffu}) {
            PpcState state;
            state.gpr[4] = left;
            state.gpr[5] = right;
            state.cr = 0x01234567u;
            state.xer = 0xe0000000u;
            const uint32_t product = static_cast<uint32_t>(uint64_t{left} * right);
            ppc_00003140(state);
            const auto check = [&](uint32_t expected) {
                Require(state.gpr[3] == expected && state.gpr[4] == left &&
                            state.gpr[5] == right && state.xer == 0xe0000000u &&
                            state.cr == (((expected == 0 ? 3u :
                                          expected < 0x80000000u ? 5u : 9u) << 28) |
                                         0x01234567u),
                        "Compiled mullw./divwu. result, CR0 or preserved registers mismatch.");
            };
            check(product);
            const auto productCr = state.cr;
            ppc_00003160(state);
            Require(state.gpr[3] == product && state.cr == productCr,
                    "Non-record mullw changed CR or produced a different product.");
            if (right == 0) {
                const auto previous = state.gpr[3];
                const auto cr = state.cr;
                Reject([&] { ppc_00003150(state); }, "zero divisor");
                Reject([&] { ppc_00003170(state); }, "zero divisor");
                Require(state.gpr[3] == previous && state.cr == cr,
                        "Undefined division partially modified destination or CR.");
            } else {
                ppc_00003150(state);
                check(left / right);
                const auto quotientCr = state.cr;
                ppc_00003170(state);
                Require(state.gpr[3] == left / right && state.cr == quotientCr,
                        "Non-record divwu changed CR or produced a different quotient.");
            }
        }
    }
}

std::function<void(PpcState&)> nativeTestAction;

void NativeTestHook(PpcState& state) {
    nativeTestAction(state);
}

void TestNativeDispatch() {
    PpcState state;
    state.memory.resize(0x5000);
    const std::vector<NativeTranslation> root = {
        {0x4000, ppc_00004000, {0x7fe802a6u, 0x4800001du, 0x38630001u,
                               0x7fe803a6u, 0x4e800020u}},
        {0x4060, ppc_00004060, {0x7fe802a6u, 0x4e800421u,
                               0x7fe803a6u, 0x4e800020u}}};
    const std::vector<NativeTranslation> module = {
        {0x4020, ppc_00004020, {0x38630002u, 0x4800001cu}},
        {0x4040, ppc_00004040, {0x38630004u, 0x4e800020u}}};
    for (const auto* bindings : {&root, &module}) {
        for (const auto& binding : *bindings) {
            for (size_t i = 0; i < binding.expectedWords.size(); ++i) {
                ppc_write_u32(state, binding.address + static_cast<uint32_t>(i * 4),
                              binding.expectedWords[i]);
            }
        }
    }
    NativeDispatcher dispatcher;
    dispatcher.Register(0, state, root);
    dispatcher.Register(1, state, module);
    unsigned previousCalls = 0;
    state.callGuest = [&previousCalls](uint32_t, PpcState&) { ++previousCalls; };
    for (const uint32_t entry : {0x4000u, 0x4060u}) {
        state.lr = 0x4ff0;
        state.ctr = 0x4020;
        state.gpr[3] = 10;
        dispatcher.Execute(state, entry, 3, 2);
        Require(state.gpr[3] == (entry == 0x4000 ? 17u : 16u) &&
                    state.pc == 0x4ff0 && state.lr == 0x4ff0,
                "Compiled direct/indirect call, tail branch or return failed.");
    }
    Require(previousCalls == 0, "Native dispatcher used an unrelated callback.");
    ppc_call_guest(state, 0);
    Require(previousCalls == 1, "Native dispatcher did not restore the prior callback.");
    Reject([&] { dispatcher.Register(1, state, module); }, "already registered");
    Reject([&] { dispatcher.Register(2, state, {module[0], module[1]}); },
           "already registered");
    Reject([&] { dispatcher.Register(2, state, {{0x4080, nullptr, {0}}}); },
           "Invalid native");
    Reject([&] { dispatcher.Register(2, state, {{0x4081, ppc_00004040, {0}}}); },
           "Invalid native");
    Reject([&] { dispatcher.Register(2, state, {{0x4080, ppc_00004040, {1}}}); },
           "code mismatch");
    Reject([&] { dispatcher.Register(2, state, {
        {0x4080, ppc_00004040, {0}}, module[0]}); }, "already registered");
    Require(!dispatcher.Contains(0x4080), "Failed native registration partially committed.");
    for (const auto limits : {std::pair<size_t, size_t>{2, 2}, {3, 1}}) {
        state.lr = 0x4ff0;
        Reject([&] { dispatcher.Execute(state, 0x4000, limits.first, limits.second); },
               limits.first == 2 ? "budget exceeded" : "depth limit");
        ppc_call_guest(state, 0);
    }
    Require(previousCalls == 3, "Failed native execution did not restore the callback.");
    ppc_write_u32(state, 0x4020, 0);
    state.lr = 0x4ff0;
    Reject([&] { dispatcher.Execute(state, 0x4000); }, "code mismatch at 0x00004020");
    ppc_write_u32(state, 0x4020, module[0].expectedWords[0]);
    dispatcher.Unregister(1);
    Require(!dispatcher.Contains(0x4020) && !dispatcher.Contains(0x4040) &&
                dispatcher.Contains(0x4000), "Native module removal left stale bindings.");
    state.lr = 0x4ff0;
    Reject([&] { dispatcher.Execute(state, 0x4000); },
           "No native translation registered at 0x00004020");
    Reject([&] { dispatcher.Unregister(1); }, "not registered");
    dispatcher.Register(1, state, module);
    state.lr = 0x4ff0;
    dispatcher.Execute(state, 0x4000);
    Require(state.pc == 0x4ff0, "Re-registering native module failed.");
    Reject([&] { dispatcher.Execute(state, 0x4001); }, "Invalid");
    state.lr = 0x4ff1;
    Reject([&] { dispatcher.Execute(state, 0x4000); }, "Invalid");
    state.lr = 0x4ff0;
    Reject([&] { dispatcher.Execute(state, 0x4000, 0); }, "Invalid");
    Reject([&] { dispatcher.Execute(state, 0x4000, 1, 0); }, "Invalid");
    dispatcher.Register(2, state, {{0x4080, NativeTestHook, {0}}});
    nativeTestAction = [&](PpcState&) { dispatcher.Unregister(1); };
    Reject([&] { dispatcher.Execute(state, 0x4080); }, "during execution");
    Require(dispatcher.Contains(0x4020), "Active native module was unregistered.");
    nativeTestAction = [&](PpcState&) { dispatcher.Register(3, state, root); };
    Reject([&] { dispatcher.Execute(state, 0x4080); }, "during execution");
    nativeTestAction = [&](PpcState&) { dispatcher.Execute(state, 0x4040); };
    Reject([&] { dispatcher.Execute(state, 0x4080); }, "reentrant");
    nativeTestAction = [](PpcState& guest) {
        PpcState unrelated;
        guest.callGuest(0x4040, unrelated);
    };
    Reject([&] { dispatcher.Execute(state, 0x4080); }, "call context");
    nativeTestAction = [](PpcState& guest) { guest.pc = 0x4080; };
    Reject([&] { dispatcher.Execute(state, 0x4080, 2); }, "budget exceeded");
    nativeTestAction = {};
    dispatcher.Unregister(2);
    state.callGuest = {};
    dispatcher.Execute(state, 0x4040);
    Require(!state.callGuest, "Native dispatcher did not restore an empty callback.");
}

} // namespace

int main() {
    try {
        TestLogicalRegisters();
        TestCarryAndSpecialRegisters();
        TestLinkedBranchAndMultiLoad();
        TestLocalIndirectBranches();
        TestComparisonAndNor();
        TestCountLeadingZeros();
        TestSignExtendHalfword();
        TestSignExtendByteAndNegate();
        TestTimeBaseAndConditionXor();
        TestSubtractImmediateCarry();
        TestSubtractExtended();
        TestMultiplyDivideWords();
        TestNativeDispatch();
    } catch (const std::exception& error) {
        std::cerr << "Generated-code test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All compiled PowerPC translation tests passed.\n";
    return 0;
}
