#include "dol_image.h"
#include "ppc_recompiler.h"
#include "ppc_state.h"
#include "rvz_image.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void WriteBigEndian32(std::vector<uint8_t>& bytes, size_t offset,
                      uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value >> 24);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 16);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 8);
    bytes[offset + 3] = static_cast<uint8_t>(value);
}

void WriteBigEndian64(std::vector<uint8_t>& bytes, size_t offset,
                      uint64_t value) {
    WriteBigEndian32(bytes, offset, static_cast<uint32_t>(value >> 32));
    WriteBigEndian32(bytes, offset + 4, static_cast<uint32_t>(value));
}

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::vector<uint8_t> MakeDol(
    const std::vector<uint32_t>& instructions,
    uint32_t loadAddress = 0x80003100u) {
    std::vector<uint8_t> bytes(0x100 + instructions.size() * 4, 0);
    WriteBigEndian32(bytes, 0, 0x100);
    WriteBigEndian32(bytes, 0x48, loadAddress);
    WriteBigEndian32(bytes, 0x90,
                     static_cast<uint32_t>(instructions.size() * 4));
    WriteBigEndian32(bytes, 0xe0, loadAddress);
    for (size_t index = 0; index < instructions.size(); ++index) {
        WriteBigEndian32(bytes, 0x100 + index * 4, instructions[index]);
    }
    return bytes;
}

void TestDolTextSectionRead() {
    std::vector<uint8_t> dol =
        MakeDol({0x38600005u, 0x3883fffeu, 0x4e800020u});
    dol.resize(dol.size() + 4, 0);
    WriteBigEndian32(dol, 0x1c, 0x10c);
    WriteBigEndian32(dol, 0x64, 0x80004000u);
    WriteBigEndian32(dol, 0xac, 4);
    WriteBigEndian32(dol, 0x10c, 0x12345678u);
    const DolImage image = DolImage::Parse(std::move(dol));
    Require(image.entryPoint() == 0x80003100u,
            "DOL entry point was not decoded.");
    Require(image.sections().size() == 2 &&
                image.sections()[0].executable &&
                !image.sections()[1].executable,
            "DOL text and data sections were not decoded.");

    const std::vector<uint32_t> words =
        image.ReadWords(0x80003100u, 12u);
    Require(words.size() == 3 && words[0] == 0x38600005u &&
                words[2] == 0x4e800020u,
            "DOL PowerPC words were not read in big-endian order.");
    try {
        image.ReadWords(0x80004000u, 4u);
        throw std::runtime_error("DOL data section was treated as code.");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find("DOL text section") !=
                    std::string::npos,
                "DOL data section was rejected unexpectedly.");
    }
}

void TestGeneratedCpp() {
    const std::string code =
        RecompileFunction(0x80003100u,
                          {0x38600005u, 0x3883fffeu, 0x4e800020u});
    Require(code.find("state.gpr[3] = state.gpr[0] + "
                      "static_cast<uint32_t>(5);") == std::string::npos &&
                code.find("state.gpr[3] = 0u + "
                          "static_cast<uint32_t>(5);") != std::string::npos,
            "addi with RA=0 did not use the architectural zero base.");
    Require(code.find("state.gpr[4] = state.gpr[3] + "
                      "static_cast<uint32_t>(-2);") != std::string::npos,
            "Signed addi immediate did not translate to C++.");
    Require(code.find("const uint32_t target_80003108 = state.lr & ~3u;") !=
                std::string::npos &&
                code.find("state.pc = target_80003108;") != std::string::npos,
            "blr did not translate to a native return.");
}

void TestLinkRegisterAndRotateInstructions() {
    const std::string code = RecompileFunction(
        0x80003100u,
        {0x7c0802a6u, 0x7c0803a6u, 0x5483063eu, 0x4e800020u});
    Require(code.find("state.gpr[0] = state.lr;") != std::string::npos,
            "mflr did not translate to a link-register read.");
    Require(code.find("state.lr = state.gpr[0];") != std::string::npos,
            "mtlr did not translate to a link-register write.");
    Require(code.find("ppc_mask32(24u, 31u)") != std::string::npos,
            "rlwinm did not translate its PowerPC bit mask.");
    Require(ppc_rotate_left32(0x12345678u, 8) == 0x34567812u &&
                ppc_mask32(24, 31) == 0xffu,
            "PowerPC rotate/mask helpers produced incorrect values.");
}

void TestInternalBranchAndUnsupportedInstruction() {
    const uint32_t branchToNext = 0x48000004u;
    const std::string code =
        RecompileFunction(0x80003100u, {branchToNext, 0x4e800020u});
    Require(code.find("goto loc_80003104;") != std::string::npos,
            "Internal PowerPC branch target was not resolved.");

    try {
        RecompileFunction(0x80003100u, {0x00000000u});
        throw std::runtime_error("Unsupported instructions were accepted.");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find("80003100") != std::string::npos,
                "Unsupported instruction error omitted its address.");
    }
}

void TestGuestMemoryAndBranchCondition() {
    PpcState state;
    state.memory.resize(8);
    ppc_write_u32(state, 2, 0x12345678u);
    Require(ppc_read_u32(state, 2) == 0x12345678u,
            "Guest memory helpers are not big-endian.");
    ppc_write_u16(state, 0, 0x8123u);
    Require(ppc_read_u16(state, 0) == 0x8123u &&
                ppc_sign_extend16(0x8123u) == 0xffff8123u,
            "PowerPC halfword helpers produced incorrect results.");
    ppc_set_cr_field(state, 0, true, false, false);
    Require((state.cr >> 31) == 1u,
            "Condition register field was not updated.");
    state.xer = 0x80000000u;
    ppc_set_cr_field(state, 1, false, false, true);
    Require(((state.cr >> 24) & 0x0fu) == 0x3u,
            "Condition-register comparison did not use XER summary overflow.");
    Require(ppc_branch_condition(state, 12u, 0u),
            "Expected CR branch condition was false.");
    state.ctr = 2;
    Require(ppc_branch_condition(state, 0x10u, 0u) && state.ctr == 1,
            "CTR branch condition did not decrement and test CTR.");
    Require(!ppc_branch_condition(state, 0x10u, 0u) && state.ctr == 0,
            "CTR branch condition did not stop branching at zero.");
    state.ctr = 1;
    Require(ppc_branch_condition(state, 0x12u, 0u) && state.ctr == 0,
            "Inverted CTR branch condition was not honored.");

    try {
        ppc_read_u32(state, 6);
        throw std::runtime_error("Out-of-range guest memory read was accepted.");
    } catch (const std::out_of_range&) {
    }
}

void TestLoadsStoresAndArithmeticTranslation() {
    const std::string code = RecompileFunction(
        0x80003100u,
        {0x8064fff0u, 0xa8a40002u, 0xb0a40004u, 0x7ca32a14u});
    Require(code.find("ppc_read_u32(state, state.gpr[4] + "
                      "static_cast<uint32_t>(-16))") != std::string::npos,
            "lwz did not translate its signed displacement.");
    Require(code.find("ppc_sign_extend16(ppc_read_u16(state, "
                      "state.gpr[4] + static_cast<uint32_t>(2)))") !=
                std::string::npos,
            "lha did not translate its signed halfword load.");
    Require(code.find("ppc_write_u16(state") != std::string::npos,
            "sth did not translate to a guest halfword store.");
    Require(code.find("state.gpr[5] = state.gpr[3] + state.gpr[5];") !=
                std::string::npos,
            "add did not translate to C++.");
}

void TestRecordArithmeticConditionRegister() {
    const std::string code =
        RecompileFunction(0x80003100u, {0x7c642a15u});
    Require(code.find("static_cast<int32_t>(state.gpr[3]) < 0") !=
                std::string::npos &&
                code.find("static_cast<int32_t>(state.gpr[3]) > 0") !=
                    std::string::npos &&
                code.find("state.gpr[3] == 0") != std::string::npos,
            "Record add did not set all CR0 comparison bits.");

    const std::string addicCode =
        RecompileFunction(0x80003100u, {0x34640001u});
    Require(addicCode.find("static_cast<int32_t>(result_80003100) < 0") !=
                std::string::npos &&
                addicCode.find("static_cast<int32_t>(result_80003100) > 0") !=
                    std::string::npos &&
                addicCode.find("result_80003100 == 0") != std::string::npos,
            "addic. did not set all CR0 comparison bits.");
}

void TestInvalidRegisterComparison() {
    for (uint32_t instruction : {0x7c232000u, 0x7c432040u, 0x7c032001u}) {
        bool rejected = false;
        try {
            RecompileFunction(0x80003100u, {instruction});
        } catch (const std::runtime_error& error) {
            Require(std::string(error.what()).find("comparison at 0x80003100") !=
                        std::string::npos,
                    "Invalid register comparison failed without its guest address.");
            rejected = true;
        }
        Require(rejected, "Reserved or 64-bit register comparison was accepted.");
    }
}

void TestInvalidCountLeadingZeros() {
    bool rejected = false;
    try {
        RecompileFunction(0x80003100u, {0x7c642834u});
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find("Invalid cntlzw instruction at 0x80003100") !=
                    std::string::npos,
                "Invalid cntlzw failed without its guest address.");
        rejected = true;
    }
    Require(rejected, "cntlzw with nonzero reserved bits was accepted.");
}

void TestInvalidSignExtendHalfword() {
    bool rejected = false;
    try {
        RecompileFunction(0x80003100u, {0x7c642f34u});
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find("Invalid extsh instruction at 0x80003100") !=
                    std::string::npos,
                "Invalid extsh failed without its guest address.");
        rejected = true;
    }
    Require(rejected, "extsh with nonzero reserved bits was accepted.");
}

void TestInvalidTimeBaseAndConditionXor() {
    for (const uint32_t instruction : {0x7c6c42e7u, 0x7c6e42e6u, 0x4cc63183u,
                                       0x44000003u, 0x4c000065u}) {
        bool rejected = false;
        try {
            RecompileFunction(0x80003100u, {instruction});
        } catch (const std::runtime_error& error) {
            Require(std::string(error.what()).find("at 0x80003100") != std::string::npos,
                    "Unsupported control instruction failed without a guest address.");
            rejected = true;
        }
        Require(rejected, "Unsupported control instruction was accepted.");
    }
}

void TestInvalidUnaryAndOverflowArithmetic() {
    for (const uint32_t instruction : {0x7c642dd6u, 0x7c642f96u, 0x7c6404d0u,
                                      0x7c6428d0u, 0x7c642f74u, 0x7c642d10u}) {
        bool rejected = false;
        try {
            RecompileFunction(0x80003100u, {instruction});
        } catch (const std::runtime_error& error) {
            Require(std::string(error.what()).find("at 0x80003100") != std::string::npos,
                    "Invalid unary/overflow arithmetic failed without a guest address.");
            rejected = true;
        }
        Require(rejected, "Reserved unary bits or unsupported overflow arithmetic was accepted.");
    }
}

void TestInvalidDolRange() {
    const DolImage image = DolImage::Parse(MakeDol({0x4e800020u}));
    try {
        image.ReadWords(0x80003100u, 8u);
        throw std::runtime_error("Out-of-range DOL read was accepted.");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find("DOL text section") !=
                    std::string::npos,
                "Out-of-range DOL read failed unexpectedly.");
    }

    try {
        DolImage::Parse(std::vector<uint8_t>(0x100, 0));
        throw std::runtime_error("DOL without sections was accepted.");
    } catch (const std::runtime_error& error) {
        Require(std::string(error.what()).find("no file-backed sections") !=
                    std::string::npos,
                "Empty DOL failed unexpectedly.");
    }
}

void TestInvalidFloatingPointInstructions() {
    for (uint32_t operation : {86u, 982u}) {
        for (uint32_t reserved : {1u, 1u << 21, 31u << 21}) {
            bool rejected = false;
            try {
                RecompileFunction(0x80003100u, {(31u << 26) | (3u << 11) |
                                              (operation << 1) | reserved});
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            Require(rejected, "Invalid cache-block instruction was accepted.");
        }
    }
    for (uint32_t spr : {952u, 956u, 953u, 954u, 957u, 958u}) {
        for (uint32_t operation : {339u, 467u}) {
            const uint32_t instruction = (31u << 26) | (3u << 21) |
                ((spr & 31u) << 16) | ((spr >> 5) << 11) |
                (operation << 1) | (operation == 467 ? 1u : 0u);
            bool rejected = false;
            try {
                RecompileFunction(0x80003100u, {instruction});
            } catch (const std::runtime_error&) {
                rejected = true;
            }
            Require(rejected, "Unsupported performance-monitor read/record form was accepted.");
        }
    }
    const std::vector<uint32_t> instructions = {
        (49u << 26) | (3u << 21),
        (53u << 26) | (3u << 21),
        (51u << 26) | (3u << 21),
        (55u << 26) | (3u << 21),
        (31u << 26) | (3u << 21) | (4u << 11) | (631u << 1),
        (31u << 26) | (3u << 21) | (4u << 11) | (759u << 1),
        (31u << 26) | (3u << 21) | (3u << 16) | (4u << 11) | (599u << 1) | 1u,
        (31u << 26) | (3u << 21) | (3u << 16) | (4u << 11) | (727u << 1) | 1u,
        (63u << 26) | (3u << 21) | (1u << 16) | (4u << 11) | (72u << 1),
        0xfdfe058eu | 0x02000000u,
        0xfdfe058eu | 0x00010000u,
        (63u << 26) | (134u << 1) | 0x00400000u,
        (63u << 26) | (134u << 1) | 0x00000800u,
        (63u << 26) | (583u << 1) | (1u << 16),
        (63u << 26) | (583u << 1) | (1u << 11),
        (63u << 26) | (38u << 1) | (1u << 16),
        (63u << 26) | (38u << 1) | (1u << 11),
        (63u << 26) | (70u << 1) | (1u << 16),
        (63u << 26) | (70u << 1) | (1u << 11),
        0xfc64286au, // fadd reserves its third source field.
        0xec642826u, // Reserved single-precision arithmetic operation.
        0xfc64282au | (1u << 6),
        0xec642824u | (1u << 6),
        (63u << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (25u << 1),
        (63u << 26) | (4u << 16) | (5u << 11) | 1u,
        (63u << 26) | (1u << 21) | (4u << 16) | (5u << 11),
        (63u << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (15u << 1),
        0x10210090u, // Reserved ps_mr source field.
        (57u << 26) | (3u << 21),
        (4u << 26) | (3u << 21) | (4u << 11) | (38u << 1),
        (4u << 26) | (3u << 21) | (3u << 16) | (4u << 11) | (6u << 1) | 1u,
        (4u << 26) | (3u << 21) | (3u << 16) | (4u << 11) | (38u << 1) | 1u,
        (4u << 26) | (3u << 21) | (3u << 16) | (4u << 11) | (7u << 1) | 1u,
        (4u << 26) | (3u << 21) | (3u << 16) | (4u << 11) | (39u << 1) | 1u,
        (4u << 26) | (3u << 21) | (4u << 11) | (39u << 1),
        (61u << 26) | (3u << 21), // Paired update stores require a nonzero base.
        0x7c70e3a7u, // GQR SPR writes have no record form.
        0x7c70e2a7u,
        0x7c78e2a7u,
        0x7c78e3a7u,
        0x7c6000a7u,
        0x7c600125u,
        0x7c6100a6u,
        0x7c600924u,
        0x7c70faa7u,
        0x7c70fba7u,
        0x7c79faa7u,
        0x7c79fba7u,
        0x7c0004adu,
        0x7c2004acu,
        0x7c0104acu,
        0x7c000cacu,
        0x4c00012du,
        0x4c20012cu,
        0x4c01012cu,
        0x4c00092cu,
        (31u << 26) | (1u << 21) | (54u << 1),
        (31u << 26) | (54u << 1) | 1u,
        (31u << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (522u << 1),
        (31u << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (650u << 1),
        0x1022182au | (1u << 6), // Paired add reserves its third source field.
        (4u << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (25u << 1),
        (4u << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (12u << 1),
        (4u << 26) | (3u << 21) | (4u << 16) | (5u << 11) | (13u << 1),
        (63u << 26) | (1u << 16) | (12u << 1),
        (63u << 26) | (1u << 16) | (26u << 1),
        0x4c401383u // CR logical instructions reserve the record/link bit.
    };
    for (uint32_t instruction : instructions) {
        bool rejected = false;
        try {
            RecompileFunction(0x80003100u, {instruction});
        } catch (const std::runtime_error& error) {
            Require(std::string(error.what()).find("at 0x80003100") != std::string::npos,
                    "Invalid/unsupported floating-point instruction omitted its guest address.");
            rejected = true;
        }
        Require(rejected, "Invalid/unsupported floating-point instruction was accepted.");
    }
}

void TestGameCubeRamAndDolLoading() {
    auto bytes = MakeDol({0x38600005u, 0x4e800020u});
    bytes.resize(0x10c, 0);
    WriteBigEndian32(bytes, 0x1c, 0x108);
    WriteBigEndian32(bytes, 0x64, 0x80300000u);
    WriteBigEndian32(bytes, 0xac, 4);
    WriteBigEndian32(bytes, 0x108, 0x12345678u);
    WriteBigEndian32(bytes, 0xd8, 0x80310000u);
    WriteBigEndian32(bytes, 0xdc, 0x20);
    auto state = DolImage::Parse(bytes).CreateGameCubeState();
    Require(state.memory.size() == kGameCubeRamSize &&
                state.memoryMap == PpcMemoryMap::GameCubeRam &&
                state.pc == 0x80003100u &&
                ppc_read_u32(state, state.pc) == 0x38600005u &&
                ppc_read_u32(state, 0x80300000u) == 0x12345678u,
            "DOL text/data were not loaded at their GameCube RAM addresses.");
    Require(ppc_read_u32(state, 0x80310000u) == 0 &&
                ppc_read_u32(state, 0x8031001cu) == 0,
            "DOL BSS was not zero-initialized.");
    ppc_write_u32(state, 0x80001000u, 0x12345678u);
    Require(ppc_read_u32(state, 0xc0001000u) == 0x12345678u &&
                ppc_read_u32(state, 0x1000u) == 0x12345678u,
            "Cached, uncached and physical RAM aliases do not share storage.");
    ppc_write_u16(state, 0xc0001002u, 0xabcd);
    Require(ppc_read_u32(state, 0x80001000u) == 0x1234abcdu,
            "Uncached RAM write did not update the cached alias.");
    ppc_write_u8(state, 0x817fffffu, 0x7a);
    Require(ppc_read_u8(state, 0xc17fffffu) == 0x7a,
            "Last GameCube RAM byte is not accessible.");
    for (uint32_t address : {0x017fffffu, 0x817fffffu, 0xc17fffffu,
                             0x81800000u, 0xc1800000u, 0xcc000000u,
                             0x40001000u, 0xffffffffu}) {
        try {
            ppc_read_u32(state, address);
            throw std::runtime_error("Out-of-RAM or crossing GameCube read was accepted.");
        } catch (const std::out_of_range&) {
        }
        try {
            ppc_write_u32(state, address, 0xffffffffu);
            throw std::runtime_error("Out-of-RAM or crossing GameCube write was accepted.");
        } catch (const std::out_of_range&) {
        }
    }
    Require(ppc_read_u8(state, 0x817fffffu) == 0x7a,
            "Rejected cross-boundary write partially modified RAM.");

    for (unsigned invalid = 0; invalid < 4; ++invalid) {
        auto corrupt = bytes;
        if (invalid == 0) {
            WriteBigEndian32(corrupt, 0x64, 0xcc000000u);
        } else if (invalid == 1) {
            WriteBigEndian32(corrupt, 0xd8, 0x817fffffu);
        } else if (invalid == 2) {
            WriteBigEndian32(corrupt, 0x64, 0xc0003100u);
        } else {
            WriteBigEndian32(corrupt, 0xe0, 0x80300000u);
        }
        bool rejected = false;
        try {
            DolImage::Parse(std::move(corrupt)).CreateGameCubeState();
        } catch (const std::exception& error) {
            const std::string message = error.what();
            const char* expected = invalid == 0 ? "not mapped" :
                                   invalid == 1 ? "boundary" :
                                   invalid == 2 ? "overlap" : "DOL text section";
            Require(message.find(expected) != std::string::npos,
                    "Invalid DOL load failed for an unexpected reason.");
            rejected = true;
        }
        Require(rejected, "DOL loader accepted invalid RAM sections, BSS or entry point.");
    }
}

void TestRvzHeaderInspection() {
    constexpr const char* path = "hmawl_recompiler_test.rvz";
    std::vector<uint8_t> rvz(0x48 + 0xdc, 0);
    rvz[0] = 'R';
    rvz[1] = 'V';
    rvz[2] = 'Z';
    rvz[3] = 1;
    WriteBigEndian32(rvz, 0x0c, 0xdc);
    WriteBigEndian64(rvz, 0x24, 0x57058000u);
    WriteBigEndian64(rvz, 0x2c, rvz.size());
    WriteBigEndian32(rvz, 0x48, 1);
    WriteBigEndian32(rvz, 0x4c, 5);
    WriteBigEndian32(rvz, 0x54, 0x20000);
    constexpr char discId[] = "GYWEE9";
    std::copy(discId, discId + 6, rvz.begin() + 0x58);
    rvz[0x5e] = 0;
    rvz[0x5f] = 0;
    constexpr char title[] = "Harvest Moon: A Wonderful Life";
    std::copy(title, title + sizeof(title) - 1, rvz.begin() + 0x78);

    {
        std::ofstream output(path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(rvz.data()),
                     static_cast<std::streamsize>(rvz.size()));
        Require(static_cast<bool>(output), "Could not write the RVZ test file.");
    }

    try {
        const RvzMetadata metadata = InspectRvz(path);
        Require(metadata.discId == "GYWEE9",
                "RVZ GameCube disc ID was not read from its header.");
        Require(metadata.title == title && metadata.discVersion == 0 &&
                    metadata.compression == 5 &&
                    metadata.chunkSize == 0x20000,
                "RVZ metadata fields were not decoded.");
        Require(metadata.discSize == 0x57058000u &&
                    metadata.fileSize == rvz.size(),
                "RVZ sizes were not decoded.");
    } catch (...) {
        std::remove(path);
        throw;
    }
    Require(std::remove(path) == 0, "Could not remove the RVZ test file.");
}

} // namespace

int main() {
    try {
        TestDolTextSectionRead();
        TestGeneratedCpp();
        TestLinkRegisterAndRotateInstructions();
        TestInternalBranchAndUnsupportedInstruction();
        TestGuestMemoryAndBranchCondition();
        TestLoadsStoresAndArithmeticTranslation();
        TestRecordArithmeticConditionRegister();
        TestInvalidRegisterComparison();
        TestInvalidCountLeadingZeros();
        TestInvalidSignExtendHalfword();
        TestInvalidTimeBaseAndConditionXor();
        TestInvalidUnaryAndOverflowArithmetic();
        TestInvalidFloatingPointInstructions();
        TestInvalidDolRange();
        TestGameCubeRamAndDolLoading();
        TestRvzHeaderInspection();
    } catch (const std::exception& error) {
        std::cerr << "Recompiler test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "All static recompiler foundation tests passed.\n";
    return 0;
}
