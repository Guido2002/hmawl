#include "disc_import.h"
#include "clz_image.h"
#include "native_dvd_reader.h"
#include "native_dvd_queue.h"
#include "native_interrupts.h"
#include "native_cache.h"
#include "dol_image.h"
#include "sha1.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <zstd.h>

namespace {

void Require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Function>
void Reject(Function function, const std::string& message) {
    try {
        function();
    } catch (const std::exception& error) {
        Require(std::string(error.what()).find(message) != std::string::npos,
                "Unexpected rejection: " + std::string(error.what()));
        return;
    }
    throw std::runtime_error("Invalid input was accepted: " + message);
}

void Write32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (size_t i = 0; i < 4; ++i) {
        bytes.at(offset + i) = static_cast<uint8_t>(value >> (24u - i * 8u));
    }
}

void TestClz() {
    std::vector<uint8_t> input(16, 0);
    Write32(input, 0, 0x434c5a00u);
    Write32(input, 4, 7);
    Write32(input, 12, 7);
    input.insert(input.end(), {8, 'a', 'b', 'c', 0xfd, 0xf0, '!'});
    Require(DecodeClz(input, 7) == std::vector<uint8_t>({'a','b','c','a','b','c','!'}),
            "CLZ literal/back-reference decoding failed.");
    Reject([&] { DecodeClz(input, 6); }, "size limit");
    auto bad = input;
    bad[3] = 1;
    Reject([&] { DecodeClz(bad, 7); }, "signature");
    for (size_t length = 0; length < input.size(); ++length) {
        const std::vector<uint8_t> truncated(input.begin(), input.begin() + length);
        Reject([&] { DecodeClz(truncated, 7); }, "truncated");
    }
    bad = input;
    bad[16] = 1;
    Reject([&] { DecodeClz(bad, 7); }, "back-reference");
    bad = input;
    bad[21] = 0xff;
    Reject([&] { DecodeClz(bad, 7); }, "back-reference");
    bad = input;
    Write32(bad, 12, 0);
    Reject([&] { DecodeClz(bad, 7); }, "output size");
    Write32(bad, 12, 8);
    Reject([&] { DecodeClz(bad, 7); }, "output size");

    std::vector<uint8_t> overlap(16, 0);
    Write32(overlap, 0, 0x434c5a00u);
    Write32(overlap, 4, 19);
    Write32(overlap, 12, 19);
    overlap.insert(overlap.end(), {2, 'x', 0xff, 0xff});
    Require(DecodeClz(overlap, 19) == std::vector<uint8_t>(19, 'x'),
            "CLZ overlapping maximum-length reference failed.");

    std::vector<uint8_t> blocks(39, 0);
    Write32(blocks, 0, 0x434c5a00u);
    Write32(blocks, 4, 4);
    Write32(blocks, 8, 32);
    Write32(blocks, 12, 1);
    blocks[17] = 'z';
    Write32(blocks, 32, 3);
    blocks[36] = 1;
    blocks[37] = 0xff;
    blocks[38] = 0xf0;
    Require(DecodeClz(blocks, 4) == std::vector<uint8_t>(4, 'z'),
            "CLZ padded blocks or cross-block reference failed.");
    bad = blocks;
    Write32(bad, 8, 0);
    Reject([&] { DecodeClz(bad, 4); }, "stride");
    Write32(bad, 8, 17);
    Reject([&] { DecodeClz(bad, 4); }, "stride");
    Write32(bad, 8, 100);
    Reject([&] { DecodeClz(bad, 4); }, "stride");
    std::vector<uint8_t> empty(12, 0);
    Write32(empty, 0, 0x434c5a00u);
    Require(DecodeClz(empty, 0).empty(), "CLZ empty output failed.");
}

void Write64(std::vector<uint8_t>& bytes, size_t offset, uint64_t value) {
    Write32(bytes, offset, static_cast<uint32_t>(value >> 32));
    Write32(bytes, offset + 4, static_cast<uint32_t>(value));
}

uint32_t Read32(const std::vector<uint8_t>& bytes, size_t offset) {
    return (static_cast<uint32_t>(bytes.at(offset)) << 24) |
           (static_cast<uint32_t>(bytes.at(offset + 1)) << 16) |
           (static_cast<uint32_t>(bytes.at(offset + 2)) << 8) |
           bytes.at(offset + 3);
}

std::vector<uint8_t> Compress(const std::vector<uint8_t>& bytes) {
    std::vector<uint8_t> result(ZSTD_compressBound(bytes.size()));
    const size_t size = ZSTD_compress(result.data(), result.size(),
                                     bytes.data(), bytes.size(), 1);
    Require(!ZSTD_isError(size), "Could not compress synthetic RVZ data.");
    result.resize(size);
    return result;
}

size_t Append(std::vector<uint8_t>& output, const std::vector<uint8_t>& bytes) {
    while (output.size() % 4 != 0) {
        output.push_back(0);
    }
    const size_t offset = output.size();
    output.insert(output.end(), bytes.begin(), bytes.end());
    return offset;
}

void HashHeaders(std::vector<uint8_t>& rvz) {
    Write64(rvz, 0x2c, rvz.size());
    const auto discHash = Sha1(rvz.data() + 0x48, 0xdc);
    std::copy(discHash.begin(), discHash.end(), rvz.begin() + 0x10);
    const auto fileHash = Sha1(rvz.data(), 0x34);
    std::copy(fileHash.begin(), fileHash.end(), rvz.begin() + 0x34);
}

std::vector<uint8_t> MakeRvz(const std::vector<uint8_t>& disc,
                              bool compression, bool packing,
                              const std::vector<uint8_t>& packedOverride = {}) {
    constexpr uint32_t chunk = 0x8000;
    const size_t count = (disc.size() + chunk - 1u) / chunk;
    std::vector<uint8_t> rvz(0x48 + 0xdc, 0);
    rvz[0] = 'R'; rvz[1] = 'V'; rvz[2] = 'Z'; rvz[3] = 1;
    Write32(rvz, 4, 0x01000000);
    Write32(rvz, 8, 0x01000000);
    Write32(rvz, 0x0c, 0xdc);
    Write64(rvz, 0x24, disc.size());
    Write32(rvz, 0x48, 1);
    Write32(rvz, 0x4c, compression ? 5 : 0);
    Write32(rvz, 0x54, chunk);
    std::copy_n(disc.data(), 0x80, rvz.data() + 0x58);
    Write32(rvz, 0xfc, 1);
    std::vector<uint8_t> raw(24, 0);
    Write64(raw, 0, 0x80);
    Write64(raw, 8, disc.size() - 0x80);
    Write32(raw, 20, static_cast<uint32_t>(count));
    const auto rawBytes = compression ? Compress(raw) : raw;
    const size_t rawOffset = Append(rvz, rawBytes);
    Write64(rvz, 0x100, rawOffset);
    Write32(rvz, 0x108, static_cast<uint32_t>(rawBytes.size()));
    Write32(rvz, 0x10c, static_cast<uint32_t>(count));
    std::vector<uint8_t> groups(count * 12, 0);
    for (size_t i = 0; i < count; ++i) {
        const size_t size = std::min<size_t>(chunk, disc.size() - i * chunk);
        std::vector<uint8_t> bytes(disc.begin() + i * chunk,
                                   disc.begin() + i * chunk + size);
        if (std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; })) {
            continue;
        }
        if (i == 0) {
            std::fill_n(bytes.begin(), 0x80, 0xcd);
        }
        if (packing) {
            if (!packedOverride.empty()) {
                bytes = packedOverride;
            } else {
                std::vector<uint8_t> packed(4, 0);
                Write32(packed, 0, static_cast<uint32_t>(size));
                packed.insert(packed.end(), bytes.begin(), bytes.end());
                bytes = std::move(packed);
            }
            Write32(groups, i * 12 + 8, static_cast<uint32_t>(bytes.size()));
        }
        const bool compressGroup = compression && i % 2 == 0;
        if (compressGroup) {
            bytes = Compress(bytes);
        }
        const size_t offset = Append(rvz, bytes);
        Write32(groups, i * 12, static_cast<uint32_t>(offset / 4));
        Write32(groups, i * 12 + 4,
                static_cast<uint32_t>(bytes.size()) | (compressGroup ? 0x80000000u : 0u));
    }
    const auto groupBytes = compression ? Compress(groups) : groups;
    const size_t groupOffset = Append(rvz, groupBytes);
    Write64(rvz, 0x110, groupOffset);
    Write32(rvz, 0x118, static_cast<uint32_t>(groupBytes.size()));
    HashHeaders(rvz);
    return rvz;
}

class Fixture {
public:
    explicit Fixture(const std::vector<uint8_t>& bytes) {
        static unsigned sequence = 0;
        path_ = std::filesystem::temp_directory_path() /
            ("hmawl-rvz-" + std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()) +
             "-" + std::to_string(++sequence) + ".rvz");
        std::ofstream output(path_, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
        output.close();
        Require(static_cast<bool>(output), "Could not write RVZ fixture.");
    }
    ~Fixture() {
        std::error_code error;
        std::filesystem::remove(path_, error);
        if (error) {
            std::cerr << "Fixture cleanup failed: " << error.message() << '\n';
        }
    }
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

std::vector<uint8_t> MakeDisc() {
    std::vector<uint8_t> disc(0x10123, 0);
    const std::string id = "GYWEE9";
    std::copy(id.begin(), id.end(), disc.begin());
    const std::string title = "Synthetic RVZ test";
    std::copy(title.begin(), title.end(), disc.begin() + 0x20);
    Write32(disc, 0x1c, 0xc2339f3du);
    Write32(disc, 0x420, 0x7ff0);
    Write32(disc, 0x424, 0x9000);
    const std::string names("\0modules\0wlp0.rel\0wlp1.rel\0wlp2.rel\0", 36);
    Write32(disc, 0x428, static_cast<uint32_t>(60 + names.size()));
    Write32(disc, 0x7ff0, 0x100);
    Write32(disc, 0x7ff0 + 0x48, 0x80003100);
    Write32(disc, 0x7ff0 + 0x90, 4);
    Write32(disc, 0x7ff0 + 0xe0, 0x80003100);
    Write32(disc, 0x80f0, 0x4e800020u);
    Write32(disc, 0x9000, 0x01000000);
    Write32(disc, 0x9008, 5);
    Write32(disc, 0x900c, 0x01000001);
    Write32(disc, 0x9014, 5);
    for (size_t i = 0; i < 3; ++i) {
        const size_t entry = 0x9000 + (i + 2) * 12;
        Write32(disc, entry, static_cast<uint32_t>(9 + i * 9));
        Write32(disc, entry + 4, static_cast<uint32_t>(0xa000 + i * 4));
        Write32(disc, entry + 8, 4);
        Write32(disc, 0xa000 + i * 4, static_cast<uint32_t>(0x12340000u + i));
    }
    std::copy(names.begin(), names.end(), disc.begin() + 0x903c);
    return disc;
}

void TestNativeDvdReader() {
    auto disc = MakeDisc();
    Write32(disc, 0x9020, 0x6000);
    for (size_t i = 0xa000; i < 0x10020; ++i) {
        disc[i] = static_cast<uint8_t>(i * 13 + 7);
    }
    Write32(disc, 0x9034, static_cast<uint32_t>(disc.size()));
    Write32(disc, 0x9038, 0);
    for (bool compression : {false, true}) {
        Fixture fixture(MakeRvz(disc, compression, true));
        RvzImage image(fixture.path());
        NativeDvdReader reader(image);
        PpcState state;
        state.memoryMap = PpcMemoryMap::GameCubeRam;
        state.memory.resize(kGameCubeRamSize, 0xa5);
        constexpr uint32_t info = 0x80001000u;
        constexpr uint32_t output = 0x80400000u;
        ppc_write_u32(state, info + 0x30, 0xa000);
        ppc_write_u32(state, info + 0x34, 0x6000);
        for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                 {0, 0x6000}, {0x5fe0, 0x3f}, {0x6000, 0}, {4, 32}}) {
            const auto before = state.memory;
            Require(reader.ReadOpenedFile(state, info, output, range.second, range.first) ==
                        range.second,
                    "Native DVD reader returned an incorrect byte count.");
            const size_t destination = ppc_memory_offset(state, output, range.second);
            Require(std::equal(disc.begin() + 0xa000 + range.first,
                               disc.begin() + 0xa000 + range.first + range.second,
                               state.memory.begin() + destination) &&
                        std::equal(before.begin(), before.begin() + destination,
                                   state.memory.begin()) &&
                        std::equal(before.begin() + destination + range.second, before.end(),
                                   state.memory.begin() + destination + range.second),
                    "Native DVD range/padding bytes or destination memory guards differ.");
        }
        const auto rejectRead = [&](uint32_t fileInfo, uint32_t destination,
                                    uint32_t length, uint32_t offset, const char* message) {
            const auto before = state.memory;
            Reject([&] { reader.ReadOpenedFile(state, fileInfo, destination, length, offset); },
                   message);
            Require(state.memory == before, "Rejected native DVD read mutated guest memory.");
        };
        rejectRead(info, output, 0x6020, 0, "DMA padding");
        rejectRead(info, output, 0, 0x6001, "DMA padding");
        rejectRead(info, output, 32, 0xffffffffu, "DMA padding");
        rejectRead(info, output, 0xffffffffu, 0, "DMA padding");
        rejectRead(info, 0x817ffff0u, 32, 0, "boundary");
        rejectRead(0xcc000000u, output, 32, 0, "not mapped");
        ppc_write_u32(state, info + 0x34, 0x6001);
        rejectRead(info, output, 32, 0, "validated disc file");
        ppc_write_u32(state, info + 0x30, static_cast<uint32_t>(disc.size()));
        ppc_write_u32(state, info + 0x34, 0);
        Require(reader.ReadOpenedFile(state, info, output, 0, 0) == 0,
                "Native DVD empty EOF read failed.");
        rejectRead(info, output, 1, 0, "beyond the disc");
    }
}

std::function<void(PpcState&)> dvdCallbackAction;

void DvdTestCallback(PpcState& state) {
    dvdCallbackAction(state);
    state.pc = state.lr;
}

void TestNativeCache() {
    PpcState state;
    state.memory.resize(0x1800000);
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.lr = 0x80003000u;
    state.xer = 0xe0000000u;
    for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
             {0x80000000u, 1}, {0x80000001u, 31}, {0xc000001fu, 2},
             {0x817fffffu, 1}, {0x017fffffu, 1}, {0xcc000000u, 0}}) {
        state.gpr[3] = range.first;
        state.gpr[4] = range.second;
        state.cr = 0x12345678u;
        const auto before = state.memory;
        NativeFlushDataRange(state);
        Require(state.memory == before && state.gpr[3] == range.first &&
                    state.gpr[4] == range.second && state.pc == state.lr &&
                    state.xer == 0xe0000000u &&
                    state.cr == ((range.second == 0 ? 3u : 5u) << 28 | 0x02345678u),
                "Coherent native cache flush changed payloads, arguments or comparison flags.");
    }
    for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
             {0xcc000000u, 32}, {0x817fffffu, 2}, {0xfffffff0u, 32}}) {
        state.gpr[3] = range.first;
        state.gpr[4] = range.second;
        const auto before = state.memory;
        state.pc = 0x801b23c8u;
        Reject([&] { NativeFlushDataRange(state); },
               range.first == 0xfffffff0u ? "overflows" :
               range.first == 0xcc000000u ? "not mapped" : "boundary");
        Require(state.memory == before && state.pc == 0x801b23c8u,
                "Invalid native cache range wrote memory or returned success.");
    }
}

void TestNativeInterrupts() {
    for (bool initial : {false, true}) {
        for (uint32_t value : {0u, 1u, 2u, 0x80000000u, 0xffffffffu}) {
            PpcState state;
            state.externalInterruptsEnabled = initial;
            state.lr = 0x80003000u;
            state.cr = 0x12345678u;
            state.xer = 0xe0000000u;
            NativeDisableInterrupts(state);
            Require(state.gpr[3] == (initial ? 1u : 0u) &&
                        !state.externalInterruptsEnabled && state.pc == state.lr &&
                        state.cr == 0x12345678u && state.xer == 0xe0000000u,
                    "Native interrupt disable lost previous state or changed CR/XER.");
            NativeDisableInterrupts(state);
            Require(state.gpr[3] == 0 && !state.externalInterruptsEnabled,
                    "Nested native interrupt disable returned an enabled state.");
            state.gpr[3] = value;
            NativeRestoreInterrupts(state);
            const uint32_t comparison = value == 0 ? 2u : value < 0x80000000u ? 4u : 8u;
            Require(state.gpr[3] == 0 && state.externalInterruptsEnabled == (value != 0) &&
                        state.cr == (((comparison | 1u) << 28) | 0x02345678u) &&
                        state.xer == 0xe0000000u && state.pc == state.lr,
                    "Native interrupt restore result, boolean/CR semantics or CPU flags differ.");
            NativeEnableInterrupts(state);
            Require(state.gpr[3] == (value != 0 ? 1u : 0u) &&
                        state.externalInterruptsEnabled,
                    "Native interrupt enable did not return its previous state.");
            state.gpr[3] = initial ? 1u : 0u;
            NativeRestoreInterrupts(state);
            Require(state.gpr[3] == 1 && state.externalInterruptsEnabled == initial,
                    "Native interrupt restoration did not restore the saved outer mask.");
        }
    }
}

void TestNativeDvdQueue() {
    const auto disc = MakeDisc();
    Fixture fixture(MakeRvz(disc, true, true));
    RvzImage image(fixture.path());
    NativeDvdReader reader(image);
    NativeDispatcher dispatcher;
    PpcState state;
    state.memoryMap = PpcMemoryMap::GameCubeRam;
    state.memory.resize(kGameCubeRamSize, 0xa5);
    constexpr uint32_t callback = 0x80002000u;
    constexpr uint32_t info = 0x80001000u;
    constexpr uint32_t output = 0x80400000u;
    ppc_write_u32(state, callback, 0x4e800020);
    const std::vector<NativeTranslation> callbacks = {{callback, DvdTestCallback, {0x4e800020}}};
    dispatcher.Register(1, state, callbacks);
    NativeDvdQueue queue(reader, dispatcher, state);
    std::vector<std::pair<uint32_t, uint32_t>> events;
    dvdCallbackAction = [&](PpcState& guest) {
        Require(!guest.externalInterruptsEnabled,
                "Native DVD callback was not delivered with cooperative interrupts masked.");
        events.push_back({guest.gpr[3], guest.gpr[4]});
        guest.gpr[0] = 0xdeadbeef;
        guest.gpr[31] = 0xdeadbeef;
        guest.cr = 0;
        guest.xer = 0;
        guest.ctr = 0;
        guest.fpr.fill({0, 0});
        guest.fpscr = 0;
        guest.gqr.fill(0);
        guest.msr = 0;
        guest.hid2 = 0;
        guest.hid0 = 0;
        guest.l2cr = 0;
        guest.performanceMonitor.fill(0);
        ppc_write_u32(guest, 0xcc003004u, 0xf0u);
        ppc_write_u16(guest, 0xcc00401cu, 0);
        ppc_write_u16(guest, 0xcc00500au, 0x604u);
        ppc_write_u32(guest, 0xcc006c00u, 0x52u);
        ppc_write_u32(guest, 0xcc006800u, 0x1070u);
        ppc_write_u32(guest, 0xcc006814u, 0x1070u);
        ppc_write_u32(guest, 0xcc006828u, 0x70u);
    };
    state.gpr[1] = 0x8035cfa0u;
    state.lr = 0x80003000u;
    state.pc = 0x80003004u;
    state.cr = 0x12345678;
    state.xer = 0xe0000000;
    state.ctr = 0x87654321;
    for (unsigned reg = 0; reg < 32; ++reg) {
        state.fpr[reg] = {UINT64_C(0x7ff0000000000001) + reg,
                          UINT64_C(0xfff8123456780000) + reg};
    }
    state.fpscr = 0xf1234567u;
    const auto floatingRegisters = state.fpr;
    for (unsigned reg = 0; reg < 8; ++reg) {
        state.gqr[reg] = 0x12345678u + reg;
    }
    const auto quantizationRegisters = state.gqr;
    state.msr = 0x2000u;
    state.hid2 = 0xa0000000u;
    state.hid0 = 0xc200u;
    state.l2cr = 0x80000000u;
    state.performanceMonitor.fill(0x12345678u);
    state.piInterruptMask = 0;
    state.miInterruptMask = 0x1fu;
    state.dspControl = 0x7fcu;
    state.aiControl = 0x5eu;
    state.exiStatus = {0x1c7fu, 0x1c7fu, 0x7fu};
    const auto monitor = state.performanceMonitor;
    const auto prepare = [&](uint32_t block, uint32_t destination, uint32_t priority) {
        const auto offset = ppc_memory_offset(state, block, 0x3c);
        std::fill(state.memory.begin() + offset, state.memory.begin() + offset + 0x3c, 0);
        ppc_write_u32(state, block + 0x30, 0xa000);
        ppc_write_u32(state, block + 0x34, 4);
        state.gpr[3] = block;
        state.gpr[4] = destination;
        state.gpr[5] = 32;
        state.gpr[6] = 0xa000;
        state.gpr[7] = callback;
        state.gpr[8] = priority;
    };
    prepare(info, output, 2);
    const auto beforeSubmit = state.memory;
    queue.Submit(state);
    state.externalInterruptsEnabled = false;
    Require(!queue.Pump() && queue.pending() == 1 && events.empty(),
            "Native DVD queue delivered a masked completion.");
    state.externalInterruptsEnabled = true;
    Require(queue.pending() == 1 && events.empty() &&
                ppc_read_u32(state, info + 0xc) == 2 &&
                ppc_read_u32(state, info + 0x20) == 0 &&
                ppc_read_u32(state, info + 0x28) == callback,
            "DVD async submission did not queue an unfinished read.");
    const size_t buffer = ppc_memory_offset(state, output, 32);
    Require(std::equal(beforeSubmit.begin() + buffer, beforeSubmit.begin() + buffer + 32,
                       state.memory.begin() + buffer),
            "DVD async submission performed an inline data transfer.");
    prepare(info + 0x100, output + 0x100, 0);
    queue.Submit(state);
    prepare(info + 0x200, output + 0x200, 0);
    queue.Submit(state);
    const std::vector<uint32_t> registers(std::begin(state.gpr), std::end(state.gpr));
    for (uint32_t expected : {info + 0x100, info + 0x200, info}) {
        Require(queue.Pump() && events.back() == std::make_pair(32u, expected) &&
                    ppc_read_u32(state, expected + 0xc) == 0 &&
                    ppc_read_u32(state, expected + 0x20) == 32 &&
                    std::equal(registers.begin(), registers.end(), std::begin(state.gpr)) &&
                    state.cr == 0x12345678 && state.xer == 0xe0000000 &&
                    state.ctr == 0x87654321 && state.lr == 0x80003000u &&
                    state.fpr == floatingRegisters && state.fpscr == 0xf1234567u &&
                    state.gqr == quantizationRegisters &&
                    state.msr == 0x2000u && state.hid2 == 0xa0000000u &&
                    state.hid0 == 0xc200u && state.l2cr == 0x80000000u &&
                    state.performanceMonitor == monitor && state.piInterruptMask == 0xf0u &&
                    state.miInterruptMask == 0 && state.dspControl == 0x6acu &&
                    state.aiControl == 0x5au &&
                    state.exiStatus[0] == 0x187au && state.exiStatus[1] == 0x187au &&
                    state.exiStatus[2] == 0x7au &&
                    state.pc == 0x80003004u && state.externalInterruptsEnabled,
                "DVD completion priority/FIFO, status, result or CPU restoration differs.");
    }
    Require(!queue.Pump() && events.size() == 3, "DVD callback was replayed or queue failed to drain.");
    Require(std::equal(disc.begin() + 0xa000, disc.begin() + 0xa020,
                       state.memory.begin() + buffer),
            "DVD queued read did not transfer exact disc bytes.");
    prepare(info, output, 0);
    queue.Submit(state);
    state.gpr[3] = 0xc0001000u;
    state.gpr[4] = callback;
    const auto beforeCancel = state.memory;
    state.piInterruptMask = 0;
    state.miInterruptMask = 0x1fu;
    state.dspControl = 0x7fcu;
    state.aiControl = 0x5eu;
    state.exiStatus = {0x1c7fu, 0x1c7fu, 0x7fu};
    queue.Cancel(state);
    Require(queue.pending() == 0 && ppc_read_u32(state, info + 0xc) == 10 &&
                events[3] == std::make_pair(0xfffffffdu, 0xc0001000u) &&
                events[4] == std::make_pair(0u, 0xc0001000u) &&
                std::equal(beforeCancel.begin() + buffer, beforeCancel.begin() + buffer + 32,
                           state.memory.begin() + buffer) && !queue.Pump(),
            "DVD queued cancellation did not deliver -3 then cancellation success without data.");
    Require(state.fpr == floatingRegisters && state.fpscr == 0xf1234567u &&
                state.gqr == quantizationRegisters &&
                state.msr == 0x2000u && state.hid2 == 0xa0000000u &&
                state.hid0 == 0xc200u && state.l2cr == 0x80000000u &&
                state.performanceMonitor == monitor && state.piInterruptMask == 0xf0u &&
                state.miInterruptMask == 0 && state.dspControl == 0x6acu &&
                state.aiControl == 0x5au && state.exiStatus[0] == 0x187au &&
                state.exiStatus[1] == 0x187au && state.exiStatus[2] == 0x7au,
            "DVD cancellation callback damaged floating-point context.");
    const auto rejectSubmit = [&](const char* message) {
        const auto before = state.memory;
        const auto pending = queue.pending();
        Reject([&] { queue.Submit(state); }, message);
        Require(state.memory == before && queue.pending() == pending,
                "Rejected DVD async submission mutated memory or pending requests.");
    };
    prepare(info, output, 4);
    rejectSubmit("priority");
    prepare(info, output, 0);
    state.gpr[7] = callback + 4;
    rejectSubmit("callback");
    prepare(info, output, 0);
    state.gpr[5] = 36;
    rejectSubmit("DMA padding");
    prepare(info, output, 0);
    state.gpr[4] = 0x817ffff0;
    rejectSubmit("boundary");
    prepare(info, output, 0);
    state.gpr[4] = info;
    rejectSubmit("overlaps");
    prepare(info, output, 0);
    queue.Submit(state);
    prepare(info + 0x100, info, 0);
    rejectSubmit("another queued command");
    state.gpr[3] = 0xc0001000u;
    rejectSubmit("already queued");
    ppc_write_u32(state, info + 0x34, 5);
    Reject([&] { queue.Pump(); }, "metadata changed");
    Require(queue.pending() == 0 && events.back() == std::make_pair(0xffffffffu, info) &&
                ppc_read_u32(state, info + 0xc) == 0xffffffffu && !queue.Pump(),
            "Failed DVD command did not become terminal and deliver exactly one failure callback.");
    for (uint32_t field : {8u, 0xcu, 0x10u, 0x14u, 0x18u, 0x20u, 0x28u, 0x30u, 0x38u}) {
        prepare(info, output, 0);
        queue.Submit(state);
        const size_t previousCount = events.size();
        ppc_write_u32(state, info + field, ppc_read_u32(state, info + field) ^ 1);
        Reject([&] { queue.Pump(); }, "metadata changed");
        Require(queue.pending() == 0 && events.size() == previousCount + 1 &&
                    events.back() == std::make_pair(0xffffffffu, info) &&
                    ppc_read_u32(state, info + 0x38) == 0,
                "Changed DVD command fields bypassed checks or redirected a failure callback.");
    }
    prepare(info, output, 0);
    queue.Submit(state);
    dvdCallbackAction = [&](PpcState& guest) {
        Reject([&] { queue.Pump(); }, "non-reentrant");
        state.externalInterruptsEnabled = true;
        guest.fpr.fill({UINT64_MAX, UINT64_MAX});
        guest.fpscr = 0;
        guest.gqr.fill(UINT32_MAX);
        guest.msr = UINT32_MAX;
        guest.hid2 = UINT32_MAX;
        guest.hid0 = UINT32_MAX;
        guest.l2cr = UINT32_MAX;
        guest.performanceMonitor.fill(UINT32_MAX);
        ppc_write_u32(guest, 0xcc003004u, 0xf0u);
        ppc_write_u16(guest, 0xcc00401cu, 0);
        ppc_write_u16(guest, 0xcc00500au, 0x604u);
        ppc_write_u32(guest, 0xcc006c00u, 0x52u);
        ppc_write_u32(guest, 0xcc006800u, 0x1070u);
        ppc_write_u32(guest, 0xcc006814u, 0x1070u);
        ppc_write_u32(guest, 0xcc006828u, 0x70u);
        throw std::runtime_error("Intentional DVD callback failure.");
    };
    state.piInterruptMask = 0;
    state.miInterruptMask = 0x1fu;
    state.dspControl = 0x7fcu;
    state.aiControl = 0x5eu;
    state.exiStatus = {0x1c7fu, 0x1c7fu, 0x7fu};
    const uint32_t previousPc = state.pc;
    const std::vector<uint32_t> beforeThrowRegisters(std::begin(state.gpr), std::end(state.gpr));
    Reject([&] { queue.Pump(); }, "Intentional DVD callback failure");
    Require(queue.pending() == 0 && !queue.Pump() && state.pc == previousPc &&
                std::equal(beforeThrowRegisters.begin(), beforeThrowRegisters.end(),
                           std::begin(state.gpr)) &&
                ppc_read_u32(state, info + 0xc) == 0 && state.externalInterruptsEnabled,
            "Throwing DVD callback was replayed, left the pump locked or corrupted CPU context.");
    Require(state.fpr == floatingRegisters && state.fpscr == 0xf1234567u &&
                state.gqr == quantizationRegisters &&
                state.msr == 0x2000u && state.hid2 == 0xa0000000u &&
                state.hid0 == 0xc200u && state.l2cr == 0x80000000u &&
                state.performanceMonitor == monitor && state.piInterruptMask == 0xf0u &&
                state.miInterruptMask == 0 && state.dspControl == 0x6acu &&
                state.aiControl == 0x5au && state.exiStatus[0] == 0x187au &&
                state.exiStatus[1] == 0x187au && state.exiStatus[2] == 0x7au,
            "Throwing DVD callback damaged floating-point context.");
    prepare(info, output, 0);
    queue.Submit(state);
    dispatcher.Unregister(1);
    Reject([&] { queue.Pump(); }, "callback");
    Require(queue.pending() == 1 && ppc_read_u32(state, info + 0xc) == 2,
            "Missing DVD callback silently consumed a queued request.");
    dispatcher.Register(1, state, callbacks);
    dvdCallbackAction = [&](PpcState&) {
        prepare(info, output, 0);
        queue.Submit(state);
    };
    Require(queue.Pump() && queue.pending() == 1,
            "DVD callback could not safely submit the next read on its completed block.");
    dvdCallbackAction = [](PpcState&) {};
    Require(queue.Pump() && !queue.Pump(), "Requeued DVD command did not complete exactly once.");
    prepare(info, output, 0);
    state.gpr[7] = 0;
    queue.Submit(state);
    Require(queue.Pump() && !queue.Pump(), "DVD null-callback request failed.");
    prepare(info, output, 0);
    state.gpr[5] = 0;
    state.gpr[7] = 0;
    const auto beforeEmpty = state.memory;
    queue.Submit(state);
    Require(queue.Pump() && ppc_read_u32(state, info + 0x20) == 0 &&
                std::equal(beforeEmpty.begin() + buffer, beforeEmpty.begin() + buffer + 32,
                           state.memory.begin() + buffer),
            "DVD zero-length async completion changed the destination.");
    state.gpr[3] = info;
    state.gpr[4] = 0;
    queue.Cancel(state);
    Require(state.gpr[3] == 1 && ppc_read_u32(state, info + 0xc) == 0,
            "DVD terminal-command cancellation changed a completed command.");
    prepare(info, output, 0);
    ppc_write_u32(state, info + 0xc, 1);
    rejectSubmit("terminal state");
    state.gpr[3] = info;
    state.gpr[4] = 0;
    Reject([&] { queue.Cancel(state); }, "unowned active");
    PpcState unrelated;
    Reject([&] { queue.Submit(unrelated); }, "different guest");
    Reject([&] { queue.Cancel(unrelated); }, "different guest");
    Reject([&] { NativeSubmitDvdRead(state); }, "not installed");
    Reject([&] { NativeCancelDvdRead(state); }, "not installed");
    prepare(info, output, 0);
    queue.Submit(state);
    dvdCallbackAction = [&](PpcState& guest) {
        events.push_back({guest.gpr[3], guest.gpr[4]});
    };
    image.Read(0x80, 32);
    std::filesystem::resize_file(fixture.path(), 0);
    const auto beforeIoError = state.memory;
    const size_t previousEvents = events.size();
    Reject([&] { queue.Pump(); }, "RVZ data is truncated");
    Require(queue.pending() == 0 && !queue.Pump() &&
                ppc_read_u32(state, info + 0xc) == 0xffffffffu &&
                ppc_read_u32(state, info + 0x20) == 0 &&
                events.size() == previousEvents + 1 &&
                events.back() == std::make_pair(0xffffffffu, info) &&
                std::equal(beforeIoError.begin() + buffer, beforeIoError.begin() + buffer + 32,
                           state.memory.begin() + buffer),
            "DVD I/O error lost its failure callback, modified payload or replayed the command.");
    dvdCallbackAction = {};
}

void TestSha1() {
    for (const auto& vector : std::vector<std::pair<std::string, std::string>>{
        {"", "da39a3ee5e6b4b0d3255bfef95601890afd80709"},
        {"abc", "a9993e364706816aba3e25717850c26c9cd0d89d"},
        {"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
         "84983e441c3bd26ebaae4aa1f95129e5e54670f1"},
        {std::string(1000000, 'a'), "34aa973cd4c4daa4f61eeb2bdbad27316534016f"}}) {
        Require(Sha1Hex(std::vector<uint8_t>(vector.first.begin(), vector.first.end())) ==
                    vector.second, "SHA-1 known-answer vector failed.");
    }
}

void TestRandomAccessAndExtraction() {
    const auto disc = MakeDisc();
    for (bool compression : {false, true}) {
        for (bool packing : {false, true}) {
            auto rvz = MakeRvz(disc, compression, packing);
            Fixture fixture(rvz);
            RvzImage image(fixture.path());
            Require(image.Read(0, disc.size()) == disc,
                    "RVZ reconstruction differs across compressed/raw/zero/final groups.");
            Require(image.Read(0x7ff8, 0x30) ==
                        std::vector<uint8_t>(disc.begin() + 0x7ff8, disc.begin() + 0x8028),
                    "Random access across a chunk boundary failed.");
            Require(image.Read(disc.size(), 0).empty(), "Empty end-of-disc read failed.");
            Reject([&] { image.Read(disc.size(), 1); }, "beyond the disc");
            Reject([&] { image.Read(UINT64_MAX, 4); }, "beyond the disc");
            const auto files = ReadGameCubeExecutables(image);
            Require(files.size() == 5 && files[0].name == "main.dol" &&
                        files[0].bytes.size() == 0x104 &&
                        files[1].name == "fst.bin" &&
                        files[4].name == "wlp2.rel" &&
                        files[4].bytes == std::vector<uint8_t>({0x12, 0x34, 0, 2}),
                    "DOL/FST/REL extraction failed.");
            Require(DolImage::Parse(files[0].bytes).ReadWords(0x80003100, 4)[0] ==
                        0x4e800020u, "Extracted DOL is not valid.");
            Reject([&] { VerifyUsExecutables(files); }, "SHA-1 mismatch");
            auto destination = fixture.path();
            destination += ".output";
            Reject([&] { ImportUsRvz(fixture.path(), destination); }, "SHA-1 mismatch");
            Require(!std::filesystem::exists(destination),
                    "Unverified game files were written to disk.");
        }
    }
}

void TestMalformedRvz() {
    const auto disc = MakeDisc();
    auto rvz = MakeRvz(disc, false, false);
    rvz[0x58] ^= 1;
    Fixture corruptHeader(rvz);
    Reject([&] { RvzImage image(corruptHeader.path()); }, "SHA-1");
    rvz = MakeRvz(disc, true, false);
    rvz.pop_back();
    Fixture truncated(rvz);
    Reject([&] { RvzImage image(truncated.path()); }, "file size");
    rvz = MakeRvz(disc, false, false);
    Write64(rvz, 0x110, UINT64_MAX);
    HashHeaders(rvz);
    Fixture tableRange(rvz);
    Reject([&] { RvzImage image(tableRange.path()); }, "beyond the input");
    rvz = MakeRvz(disc, false, false);
    Write32(rvz, 0x10c, UINT32_MAX);
    HashHeaders(rvz);
    Fixture counts(rvz);
    Reject([&] { RvzImage image(counts.path()); }, "table counts");
    rvz = MakeRvz(disc, false, false);
    Write32(rvz, 8, 0x02000000);
    HashHeaders(rvz);
    Fixture version(rvz);
    Reject([&] { RvzImage image(version.path()); }, "version");
    rvz = MakeRvz(disc, false, false);
    Write32(rvz, 0x54, 0x9000);
    HashHeaders(rvz);
    Fixture chunk(rvz);
    Reject([&] { RvzImage image(chunk.path()); }, "chunk size");

    std::vector<uint8_t> malformedPacked(4, 0);
    Write32(malformedPacked, 0, 0x80008000);
    Fixture seed(MakeRvz(disc, true, true, malformedPacked));
    RvzImage image(seed.path());
    Reject([&] { image.Read(0x100, 1); }, "seed is truncated");

    for (unsigned variant = 0; variant < 4; ++variant) {
        rvz = MakeRvz(disc, false, false);
        const size_t groupTable = Read32(rvz, 0x114);
        if (variant == 0) {
            Write32(rvz, 0x124 + 16, 3);
        } else if (variant == 1) {
            Write32(rvz, 0x124 + 20, 2);
        } else if (variant == 2) {
            Write32(rvz, groupTable, UINT32_MAX);
        } else {
            Write32(rvz, groupTable + 4, 1);
        }
        HashHeaders(rvz);
        Fixture invalid(rvz);
        Reject([&] {
            RvzImage invalidImage(invalid.path());
            invalidImage.Read(0x100, 1);
        }, variant == 0 ? "raw entry" : variant == 1 ? "group count" :
           variant == 2 ? "data group" : "block size mismatch");
    }
    rvz = MakeRvz(disc, true, false);
    const size_t compressedRaw = Read32(rvz, 0x104);
    rvz[compressedRaw] ^= 0xff;
    Fixture corruptZstd(rvz);
    Reject([&] { RvzImage invalidImage(corruptZstd.path()); }, "Zstandard");

    for (const auto& packed : std::vector<std::vector<uint8_t>>{
             {0, 0}, {0, 0, 0x80, 0}, {0, 0, 0, 1, 0x7f},
             {0, 0, 0x80, 1}}) {
        Fixture invalid(MakeRvz(disc, true, true, packed));
        RvzImage invalidImage(invalid.path());
        Reject([&] { invalidImage.Read(0x100, 1); },
               packed.size() == 2 ? "header is truncated" :
               packed.size() == 5 ? "size does not match" :
               packed.back() == 1 ? "invalid output size" : "literal is truncated");
    }
}

void TestMalformedFst() {
    for (unsigned variant = 0; variant < 4; ++variant) {
        auto disc = MakeDisc();
        if (variant == 0) {
            Write32(disc, 0x9008, UINT32_MAX);
        } else if (variant == 1) {
            Write32(disc, 0x9010, 1);
        } else if (variant == 2) {
            Write32(disc, 0x9028, 0xffff0000u);
        } else {
            Write32(disc, 0x9024, 9);
        }
        Fixture fixture(MakeRvz(disc, true, true));
        RvzImage image(fixture.path());
        Reject([&] { ReadGameCubeExecutables(image); },
               variant == 0 ? "root entry" : variant == 1 ? "hierarchy" :
               variant == 2 ? "beyond the disc" : "duplicated");
    }
}

void TestDiscFiles() {
    for (bool compression : {false, true}) {
        Fixture fixture(MakeRvz(MakeDisc(), compression, true));
        RvzImage image(fixture.path());
        const auto entries = ReadGameCubeFileTable(image);
        Require(entries.size() == 3 && entries[0].path == "modules/wlp0.rel" &&
                    entries[0].offset == 0xa000 && entries[0].size == 4 &&
                    entries[2].path == "modules/wlp2.rel",
                "Disc file table lost its hierarchy, order or range.");
        const auto file = ReadGameCubeFile(image, "modules/wlp1.rel");
        Require(file.name == "modules/wlp1.rel" && Read32(file.bytes, 0) == 0x12340001u,
                "Named disc file read returned incorrect bytes.");
        for (const auto& path : {"wlp1.rel", "/modules/wlp1.rel", "modules\\wlp1.rel",
                                 "modules/../wlp1.rel", "modules", "modules/WLP1.REL", ""}) {
            Reject([&] { ReadGameCubeFile(image, path); }, "not found");
        }
    }
    for (unsigned variant = 0; variant < 9; ++variant) {
        auto disc = MakeDisc();
        const size_t entry = 0x9018;
        if (variant == 0) {
            Write32(disc, entry, 0x02000009);
        } else if (variant == 1) {
            Write32(disc, entry, 0x00ffffff);
        } else if (variant == 2) {
            disc[0x903c + 9] = '/';
        } else if (variant == 3) {
            disc[0x903c + 9] = 0;
        } else if (variant == 4) {
            Write32(disc, 0x428, 60 + 17);
        } else if (variant == 5) {
            Write32(disc, 0x9014, 6);
        } else if (variant == 6) {
            Write32(disc, 0x9024, 9);
        } else if (variant == 7) {
            Write32(disc, entry + 4, static_cast<uint32_t>(disc.size()));
            Write32(disc, entry + 8, 0);
        } else {
            Write32(disc, entry + 4, static_cast<uint32_t>(disc.size()));
            Write32(disc, entry + 8, 1);
        }
        Fixture fixture(MakeRvz(disc, true, true));
        RvzImage image(fixture.path());
        if (variant == 7) {
            Require(ReadGameCubeFile(image, "modules/wlp0.rel").bytes.empty(),
                    "Zero-byte file at the disc boundary could not be read.");
        } else {
            Reject([&] { ReadGameCubeFileTable(image); },
                variant == 0 || variant == 1 ? "type or name offset" :
                variant == 2 || variant == 3 ? "invalid filename" :
                variant == 4 ? "null-terminated" : variant == 5 ? "hierarchy" :
                variant == 6 ? "duplicated" : "beyond the disc");
        }
    }
    auto disc = MakeDisc();
    Write32(disc, 0x9014, 4);
    Write32(disc, 0x9030, 9);
    Fixture repeatedName(MakeRvz(disc, false, false));
    RvzImage image(repeatedName.path());
    const auto entries = ReadGameCubeFileTable(image);
    Require(entries.size() == 3 && entries[0].path == "modules/wlp0.rel" &&
                entries[2].path == "wlp0.rel" &&
                Read32(ReadGameCubeFile(image, "wlp0.rel").bytes, 0) == 0x12340002u,
            "FST directory unwinding or distinct same-basename paths failed.");
    Reject([&] { ReadGameCubeExecutables(image); }, "duplicated");
}

void TestPrngPacking() {
    auto disc = MakeDisc();
    disc.resize(0x8000, 1);
    std::vector<uint8_t> packed(5 + 4 + 68, 0);
    Write32(packed, 0, 1);
    packed[4] = 0x7a;
    Write32(packed, 5, 0x80007fffu);
    Fixture fixture(MakeRvz(disc, true, true, packed));
    RvzImage image(fixture.path());
    const auto bytes = image.Read(0x80, 0x7f80);
    Require(std::all_of(bytes.begin(), bytes.end(), [](uint8_t b) { return b == 0; }),
            "Zero-seed PRNG reconstruction failed.");
    for (size_t i = 0; i < 17; ++i) {
        Write32(packed, 9 + i * 4, 0x12345678u ^ static_cast<uint32_t>(i));
    }
    for (bool compression : {false, true}) {
        Fixture nonzero(MakeRvz(disc, compression, true, packed));
        RvzImage nonzeroImage(nonzero.path());
        Require(Sha1Hex(nonzeroImage.Read(0x80, 0x7f80)) ==
                    "f0e9e4be71900f044ebd5c4573559de1e1eff969",
                "Nonzero-seed PRNG golden vector failed (byte shift, warmup, skip or rollover).");
    }
}

} // namespace

int main() {
    try {
        TestClz();
        TestNativeDvdReader();
        TestNativeInterrupts();
        TestNativeCache();
        TestNativeDvdQueue();
        TestSha1();
        TestRandomAccessAndExtraction();
        TestMalformedRvz();
        TestMalformedFst();
        TestDiscFiles();
        TestPrngPacking();
    } catch (const std::exception& error) {
        std::cerr << "Importer test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All RVZ importer, native DVD reader and cooperative queue tests passed.\n";
    return 0;
}
