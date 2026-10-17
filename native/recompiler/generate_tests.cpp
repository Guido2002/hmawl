#include "dol_image.h"
#include "ppc_recompiler.h"
#include "dsp_init_recompiler.h"
#include "sha1.h"
#include "us_module_fixture.h"

#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void Emit(std::ostream& output, uint32_t address,
          const std::vector<uint32_t>& instructions, RecompileOptions options = {}) {
    output << RecompileFunction(address, instructions, options) << '\n';
}

void EmitNativeBinding(std::ostream& functions, std::ostream& bindings,
                       const PpcState& state, uint32_t address, uint32_t size,
                       const char* service = nullptr) {
    std::vector<uint32_t> words;
    for (uint32_t offset = 0; offset < size; offset += 4) {
        words.push_back(ppc_read_u32(state, address + offset));
    }
    bindings << "{" << address << "u, ";
    if (service) {
        bindings << service;
    } else {
        Emit(functions, address, words);
        bindings << "ppc_" << std::hex << std::uppercase << address << std::dec;
    }
    bindings << ", {";
    for (const auto word : words) {
        bindings << word << "u, ";
    }
    bindings << "}}";
}

uint32_t XForm(unsigned source, unsigned destination, unsigned right,
               unsigned opcode, bool record = false) {
    return (31u << 26) | (source << 21) | (destination << 16) |
           (right << 11) | (opcode << 1) | static_cast<unsigned>(record);
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 2 && !(argc == 4 && (std::string(argv[1]) == "--startup" ||
                                       std::string(argv[1]) == "--rel"))) {
            throw std::runtime_error(
                "Expected <synthetic-output>, --startup <US-main.dol> <output>, or --rel <US-directory> <output>.");
        }
        std::string generated = "#include \"ppc_state.h\"\n\n";
        if (argc == 4 && std::string(argv[1]) == "--rel") {
            const auto fixture = LoadUsModuleFixture(argv[2]);
            std::ostringstream functions;
            std::ostringstream bindings;
            bindings << "#include \"native_dispatch.h\"\n"
                        "static std::vector<NativeRelTranslation> UsRelTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x81000194u, 0x1c}, {0x810001f0u, 0x14},
                     {0x81001f24u, 0x274}, {0x81003d48u, 0x388},
                     {0x81007184u, 0x408}, {0x8100ad84u, 0x3c4},
                     {0x8100d300u, 0x35c}, {0x8100e56cu, 0x134},
                     {0x81010628u, 0x2ec}, {0x81013164u, 0x2ac},
                     {0x810198d8u, 0x220}, {0x8101d414u, 0x3c8},
                     {0x8101da20u, 0xc8}, {0x81020ac0u, 0x630},
                     {0x8102adb8u, 0x3fa0},
                     {0x8101d7fcu, 0x50}, {0x8101d84cu, 0x4c},
                     {0x8101d948u, 0xd8}, {0x8102ef8cu, 0x28},
                     {0x8102efb4u, 0x60}, {0x8101301cu, 0x48}}) {
                bindings << "        {1, " << range.first -
                    fixture.modules[0].sectionAddresses[1] << "u, ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << "},\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeTranslation> UsRelDolTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x80181184u, 0x24},
                     {0x80235e98u, 0x4c}, {0x80235ea8u, 0x3c},
                     {0x80235eb4u, 0x30}, {0x80235ec8u, 0x1c},
                     {0x80235ee4u, 0x4c}, {0x80235ef4u, 0x3c},
                     {0x80235f00u, 0x30}, {0x80235f14u, 0x1c}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeTranslation> UsAllocatorTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x80178558u, 0xb4}, {0x80178974u, 0x30},
                     {0x801789a4u, 0xa0}, {0x80178e88u, 0x170},
                     {0x80179168u, 0x1dc},
                     {0x80179ee8u, 0x30}, {0x80179c90u, 0x30},
                     {0x80178884u, 8},
                     {0x8017888cu, 0x1c}, {0x801788a8u, 0x54},
                     {0x801788fcu, 0x24}, {0x80178920u, 0x54},
                     {0x80179734u, 0x10},
                     {0x80235ed4u, 0x10}, {0x80235f20u, 0x10},
                     {0x801b7c08u, 8}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeRelTranslation> UsSceneTranslations() {\n"
                        "    return {\n";
            // Integer prefixes stop before the first untranslated floating-point instruction.
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x810025ccu, 0x1bc}, {0x810042dcu, 0x260},
                     {0x81008a94u, 0x1b8}}) {
                bindings << "        {1, " << range.first -
                    fixture.modules[0].sectionAddresses[1] << "u, ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << "},\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeTranslation> UsSceneDolTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x80012800u, 0x44}, {0x80010034u, 0x24},
                     {0x80010004u, 0x18}, {0x800054e4u, 0x30},
                     {0x80005514u, 0xb8}, {0x80235ec4u, 0x20},
                     {0x80235f10u, 0x20},
                     {0x80027a4cu, 0x8c}, {0x80027db0u, 0x68}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeTranslation> UsSceneDependencyTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x8000fee8u, 0x64}, {0x8000ffb0u, 0x14},
                     {0x8017cd5cu, 0x20}, {0x8017d0ecu, 0x58},
                     {0x8017d3c4u, 0x24}, {0x8000c4c4u, 0x68},
                     {0x80009904u, 0x34}, {0x8000b4c4u, 0x1e0},
                     {0x80189644u, 0x30}, {0x80189944u, 0xe4},
                     {0x8000c45cu, 0x68}, {0x8000c3f4u, 0x68},
                     {0x80199eecu, 0x70},
                     {0x80235eb8u, 0x2c}, {0x80235f04u, 0x2c}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeRelTranslation> UsSceneDependencyRelTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x8101ddb0u, 0x3b0}, {0x81020970u, 0x68},
                     {0x810209d8u, 0x34}}) {
                bindings << "        {1, " << range.first -
                    fixture.modules[0].sectionAddresses[1] << "u, ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << "},\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeTranslation> UsArchiveTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x80203b4cu, 0xa4}, {0x80203bf0u, 0x5c},
                     {0x80203c4cu, 0x22c}, {0x80203e78u, 0x14},
                     {0x80203e8cu, 8}, {0x80203e94u, 8},
                     {0x80199f5cu, 0x5c}, {0x80199fb8u, 0x5c}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeTranslation> UsClzTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x801782f4u, 0x44}, {0x80178338u, 0x44},
                     {0x8017837cu, 0xd4}, {0x80178450u, 0xa0},
                     {0x80235eccu, 0x18}, {0x80235f18u, 0x18}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n";
            bindings << "static std::vector<NativeTranslation> UsDvdTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x801ba340u, 0x38}, {0x801ba378u, 0x2f4},
                     {0x801ba66cu, 0xc8}, {0x801ba758u, 0x160},
                     {0x801ba8b8u, 0xc4}, {0x801ba97cu, 0x68},
                     {0x802374b0u, 0x24},
                     {0x8017ad8cu, 0x60}, {0x8017ae48u, 0xb0},
                     {0x8017aef8u, 0x3c}, {0x8017b2dcu, 0x30}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n";
            bindings << "#include \"native_dvd_queue.h\"\n"
                        "static std::vector<NativeTranslation> UsDvdAsyncTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x801ba9dcu, 0xc0}, {0x801baa9cu, 0x30}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n"
                        "static std::vector<NativeTranslation> UsDvdHostServices() {\n"
                        "    return {\n        ";
            EmitNativeBinding(functions, bindings, fixture.state, 0x801bc9d8u, 0xdc,
                              "NativeSubmitDvdRead");
            bindings << ",\n        ";
            EmitNativeBinding(functions, bindings, fixture.state, 0x801bd1ccu, 0x27c,
                              "NativeCancelDvdRead");
            bindings << ",\n    };\n}\n";
            bindings << "#include \"native_interrupts.h\"\n"
                        "static std::vector<NativeTranslation> UsInterruptServices() {\n"
                        "    return {\n        ";
            EmitNativeBinding(functions, bindings, fixture.state, 0x801b3a44u, 0x14,
                              "NativeDisableInterrupts");
            bindings << ",\n        ";
            EmitNativeBinding(functions, bindings, fixture.state, 0x801b3a58u, 0x14,
                              "NativeEnableInterrupts");
            bindings << ",\n        ";
            EmitNativeBinding(functions, bindings, fixture.state, 0x801b3a6cu, 0x24,
                              "NativeRestoreInterrupts");
            bindings << ",\n    };\n}\n"
                        "static std::vector<NativeTranslation> UsGameDvdReadTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x801bd074u, 0x4c}, {0x8017b07cu, 0xe4},
                     {0x8017acf0u, 0x9c}, {0x8017b30cu, 0x1e0},
                     {0x8017b4ecu, 0x6c}, {0x8017b558u, 0x10}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            // Reuse the original register helpers already emitted for the allocator.
            bindings << "        ";
            EmitNativeBinding(functions, bindings, fixture.state, 0x80235ed4u, 0x10,
                              "ppc_80235ED4");
            bindings << ",\n        ";
            EmitNativeBinding(functions, bindings, fixture.state, 0x80235f20u, 0x10,
                              "ppc_80235F20");
            bindings << ",\n    };\n}\n";
            bindings << "#include \"native_cache.h\"\n"
                        "static std::vector<NativeTranslation> UsDecodeQueueTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x8019a5e0u, 0xc}, {0x8019a5ecu, 0x24},
                     {0x8019a610u, 0x4c}, {0x8019a65cu, 0xc},
                     {0x8019a6bcu, 0x13c}, {0x80190ef4u, 0x30},
                     {0x80190944u, 0x70}, {0x80190f54u, 0x24},
                     {0x8019a480u, 0x108}, {0x801786a0u, 0x34},
                     {0x80178d7cu, 0xc0}, {0x80178ff8u, 0x170},
                     {0x80179344u, 0x1dc},
                     {0x80190f24u, 0x30}, {0x8017860cu, 0x54},
                     {0x80178660u, 0x40}, {0x80178a44u, 0x12c},
                     {0x80178b70u, 0x164}, {0x80178cd4u, 0x1c},
                     {0x80178cf0u, 0x8c}, {0x80235ec4u, 0x20},
                     {0x80235f10u, 0x20}}) {
                bindings << "        ";
                if (range.first >= 0x80235ec4u) {
                    EmitNativeBinding(functions, bindings, fixture.state, range.first,
                                      range.second, range.first == 0x80235ec4u ?
                                          "ppc_80235EC4" : "ppc_80235F10");
                } else {
                    EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                }
                bindings << ",\n";
            }
            bindings << "        ";
            EmitNativeBinding(functions, bindings, fixture.state, 0x801b23c8u, 0x30,
                              "NativeFlushDataRange");
            bindings << ",\n    };\n}\n";
            bindings << "static std::vector<NativeTranslation> UsDvdCloseTranslations() {\n"
                        "    return {\n";
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                     {0x801ba734u, 0x24}, {0x801bd448u, 0xac},
                     {0x801bd4f4u, 0x24}, {0x801b7a40u, 0x104}}) {
                bindings << "        ";
                EmitNativeBinding(functions, bindings, fixture.state, range.first, range.second);
                bindings << ",\n";
            }
            bindings << "    };\n}\n";
            generated += functions.str() + bindings.str();
        } else if (argc == 4) {
            std::ifstream input(argv[2], std::ios::binary);
            if (!input) {
                throw std::runtime_error("Could not open startup test DOL.");
            }
            const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
            if (Sha1Hex(bytes) != "1ccfd9dfb5c250c2f45c70c74cc45e5d88d22374") {
                throw std::runtime_error("Startup tests require the verified US revision 0 DOL.");
            }
            const auto image = DolImage::Parse(bytes);
            for (const auto& range : std::vector<std::pair<uint32_t, uint32_t>>{
                {0x80003154u, 0x110}, {0x800032b0u, 0x90}, {0x80003100u, 0x40},
                {0x80003140u, 12}, {0x8000314cu, 8}, {0x800031f8u, 0x68},
                {0x80003488u, 0x24}, {0x80003340u, 0xc0},
                {0x800054e4u, 0x30}, {0x80005514u, 0xb8},
                {0x800055ccu, 0x50}, {0x801b0bccu, 0x88},
                {0x801b0b44u, 0x110}, {0x801b15a4u, 0x24},
                {0x801b0b2cu, 0x128},
                {0x801b0ae4u, 8}, {0x801b0aecu, 8},
                {0x801b1584u, 0x54}, {0x801b24e0u, 0x10},
                {0x801b24f0u, 0x14}, {0x801b2388u, 0x14},
                {0x801b0a28u, 8}, {0x801b0a30u, 8},
                {0x80003400u, 0x24},
                {0x801b0a18u, 8}, {0x801b0a20u, 8},
                {0x801b0a38u, 8}, {0x801b0a40u, 8},
                {0x801b27e4u, 0x98}, {0x801b29dcu, 0xf4},
                {0x801b34ecu, 0x218}, {0x801b3a44u, 0x14},
                {0x801b3a6cu, 0x24}, {0x801d33e4u, 0x50},
                {0x801d3348u, 0x28},
                {0x801b0de0u, 0x18c}, {0x801b119cu, 0x1c},
                {0x801b0f6cu, 0x1c}, {0x801b32f8u, 0x48},
                {0x801f7440u, 0x64},
                {0x801f74a4u, 0xa0}, {0x801f7614u, 0xf4},
                {0x801f7824u, 0x3b0}, {0x801f61f8u, 0xf4},
                {0x801f6e54u, 0x12c}, {0x801f77e4u, 0x18},
                {0x801b4c90u, 0x18},
                {0x801b3a90u, 0x1c}, {0x801b3aacu, 0x14},
                {0x801b3ac0u, 0x74},
                {0x801b3b08u, 4},
                {0x801b3b34u, 0x2d8}, {0x801b3e0cu, 0x88},
                {0x801b6988u, 0x64},
                {0x801b1618u, 0x58}, {0x801b147cu, 0x14},
                {0x801b5550u, 0x84},
                {0x801b11b8u, 0x280}, {0x801b1460u, 0x1c},
                {0x801d33c8u, 0x1c},
                {0x801b2428u, 0x2c}, {0x801b24acu, 0x34},
                {0x801b0e64u, 0xf8},
                {0x801b20d4u, 8}, {0x801b20dcu, 8},
                {0x801b20e4u, 8}, {0x801b20ecu, 8},
                {0x801b0a6cu, 8}, {0x801b0a74u, 8}, {0x801b0a7cu, 8},
                {0x801b0a84u, 8}, {0x801b0a8cu, 8}, {0x801b0a94u, 8},
                {0x801b7bf0u, 0x18}, {0x801b7c10u, 0x64},
                {0x801b0afcu, 0x28}, {0x801b0b24u, 8},
                {0x801b0a9cu, 0x20}, {0x801b0abcu, 0x28},
                {0x80235e00u, 0x4c}, {0x80235e4cu, 0x4c}}) {
                generated += RecompileFunction(range.first,
                    image.ReadWords(range.first, range.second)) + '\n';
            }
        } else {
            std::ostringstream output;
            for (unsigned index = 0; index < 32; ++index) {
                const unsigned operation = (index % 16) / 4;
                const unsigned xo = operation == 0 ? 18 : operation == 1 ? 20 : operation == 2 ? 21 : 25;
                const unsigned destination = index % 2 ? 4 : 3;
                const uint32_t word = ((index < 16 ? 59u : 63u) << 26) | (destination << 21) |
                    (4u << 16) | (xo == 25 ? (5u << 6) : (5u << 11)) |
                    (xo << 1) | ((index % 4) / 2);
                Emit(output, 0xf000u + index * 16, {word});
            }
            output << "static void (*const FloatArithmeticTestFunctions[])(PpcState&) = {\n";
            for (unsigned index = 0; index < 32; ++index) {
                output << "ppc_0000" << std::hex << std::uppercase << (0xf000u + index * 16)
                       << ",\n" << std::dec;
            }
            output << "};\n";
            const unsigned pairedOps[] = {18, 20, 21, 25, 12, 13};
            for (unsigned index = 0; index < 36; ++index) {
                const auto xo = pairedOps[index / 6];
                const unsigned destination = 3 + index % 3;
                const bool multiply = xo == 25 || xo == 12 || xo == 13;
                Emit(output, 0xf600u + index * 16,
                     {(4u << 26) | (destination << 21) | (4u << 16) |
                      (multiply ? (5u << 6) : (5u << 11)) | (xo << 1) |
                      ((index % 6) / 3)});
            }
            output << "static void (*const PairedArithmeticTestFunctions[])(PpcState&) = {\n";
            for (unsigned index = 0; index < 36; ++index) {
                output << "ppc_0000" << std::hex << std::uppercase << (0xf600u + index * 16)
                       << ",\n" << std::dec;
            }
            output << "};\n";
            const unsigned horizontalOps[] = {28, 29, 10, 11};
            for (unsigned index = 0; index < 32; ++index) {
                Emit(output, 0xf900u + index * 16,
                     {(4u << 26) | ((3 + index % 4) << 21) | (4u << 16) |
                      (5u << 11) | (6u << 6) | (horizontalOps[index / 8] << 1) |
                      ((index % 8) / 4)});
            }
            output << "static void (*const PairedFusedSumTestFunctions[])(PpcState&) = {\n";
            for (unsigned index = 0; index < 32; ++index) {
                output << "ppc_0000" << std::hex << std::uppercase << (0xf900u + index * 16)
                       << ",\n" << std::dec;
            }
            output << "};\n";
            Emit(output, 0xfb00u, {0x4c401382u});
            Emit(output, 0xfb10u, {(19u << 26) | (7u << 21) | (7u << 16) | (8u << 11) | (449u << 1)});
            Emit(output, 0xfb20u, {(19u << 26) | (8u << 21) | (7u << 16) | (8u << 11) | (449u << 1)});
            for (unsigned index = 0; index < 32; ++index) {
                Emit(output, 0xfc00u + index * 16,
                     {((index < 16 ? 59u : 63u) << 26) | ((3 + index % 4) << 21) |
                      (4u << 16) | (5u << 11) | (6u << 6) | ((28u + (index / 8) % 2) << 1) |
                      ((index % 8) / 4)});
            }
            output << "static void (*const ScalarFusedTestFunctions[])(PpcState&) = {\n";
            for (unsigned index = 0; index < 32; ++index) {
                output << "ppc_0000" << std::hex << std::uppercase << (0xfc00u + index * 16)
                       << ",\n" << std::dec;
            }
            output << "};\n";
            for (unsigned index = 0; index < 24; ++index) {
                const unsigned primary[] = {4, 59, 63};
                Emit(output, 0xfe00u + index * 16,
                     {(primary[index / 8] << 26) | ((3 + index % 4) << 21) |
                      (4u << 16) | (5u << 11) | (6u << 6) | ((30u + (index / 4) % 2) << 1) |
                      (index % 2)});
            }
            output << "static void (*const NegativeFusedTestFunctions[])(PpcState&) = {\n";
            for (unsigned index = 0; index < 24; ++index) {
                output << "ppc_0000" << std::hex << std::uppercase << (0xfe00u + index * 16)
                       << ",\n" << std::dec;
            }
            output << "};\n";
            for (unsigned index = 0; index < 8; ++index) {
                Emit(output, 0xff80u + index * 16,
                     {(63u << 26) | ((index % 2 ? 4u : 3u) << 21) | (4u << 11) |
                      ((index < 4 ? 26u : 12u) << 1) | ((index % 4) / 2)});
            }
            output << "static void (*const EstimateRoundTestFunctions[])(PpcState&) = {\n";
            for (unsigned index = 0; index < 8; ++index) {
                output << "ppc_0000" << std::hex << std::uppercase << (0xff80u + index * 16)
                       << ",\n" << std::dec;
            }
            output << "};\n";
            for (unsigned field = 0; field < 8; ++field) {
                Emit(output, 0xf200u + field * 16,
                     {(63u << 26) | (field << 23) | (4u << 16) | (5u << 11)});
            }
            output << "static void (*const FloatCompareTestFunctions[])(PpcState&) = {\n";
            for (unsigned field = 0; field < 8; ++field) {
                output << "ppc_0000" << std::hex << std::uppercase << (0xf200u + field * 16)
                       << ",\n" << std::dec;
            }
            output << "};\n";
            Emit(output, 0xf280, {(63u << 26) | (3u << 21) | (4u << 11) | (15u << 1) | 1u});
            Emit(output, 0xf290, {(63u << 26) | (3u << 23) | (4u << 16) | (5u << 11) | (32u << 1)});
            Emit(output, 0xb400u, {0x38630001u, 0x4200fffcu, 0x38800001u}, {true, true});
            Emit(output, 0xb420u, {0x48000000u, 0x38800001u}, {true, true});
            Emit(output, 0xb430u, {0x4e800420u, 0x38800001u}, {true, true});
            Emit(output, 0xb440u, {0x4d800020u, 0x38800001u}, {true, true});
            Emit(output, 0xb450u, {0x41820000u, 0x38800001u}, {true, true});
            Emit(output, 0xb460u, {0x48000009u, 0x38800001u, 0x38800002u}, {true, true});
            Emit(output, 0xb480u, {0x4e800421u, 0x38800001u}, {true, true});
            Emit(output, 0xb490u, {0x4e800021u, 0x38800001u}, {true, true});
            Emit(output, 0xb4a0u, {0x38630001u, 0x80640000u}, {false, true});
            Emit(output, 0xb4c0u, {0x42000009u, 0x38800001u, 0x38800002u}, {true, true});
            Emit(output, 0xb4e0u, {0x4d820420u, 0x38800001u}, {true, true});
            Emit(output, 0xb500u, {XForm(3, 4, 5, 11, false)});
            Emit(output, 0xb510u, {XForm(3, 4, 5, 11, true)});
            Emit(output, 0xb520u, {XForm(3, 4, 5, 792, false)});
            Emit(output, 0xb530u, {XForm(3, 4, 5, 792, true)});
            Emit(output, 0xb540u, {XForm(3, 4, 5, 824, false)});
            Emit(output, 0xb550u, {XForm(3, 4, 5, 824, true)});
            Emit(output, 0xb560u, {XForm(3, 4, 5, 316, false)});
            Emit(output, 0xb570u, {XForm(3, 4, 5, 316, true)});
            Emit(output, 0xb580u, {XForm(3, 4, 5, 8, false)});
            Emit(output, 0xb590u, {XForm(3, 4, 5, 8, true)});
            Emit(output, 0xb5a0u, {XForm(0, 4, 5, 470, false)});
            Emit(output, 0xb5b0u, {0x44000002u}, {true, true});
            Emit(output, 0xb5c0u, {0x4c000064u}, {true, true});
            Emit(output, 0xb5d0u, {0x44000002u, 0x38630001u});
            Emit(output, 0xb600u, {XForm(3, 0, 0, 19)});
            Emit(output, 0xb610u, {(31u << 26) | (3u << 21) | (0x5au << 12) | (144u << 1)});
            Emit(output, 0xb620u, {XForm(3, 4, 0, 202, true)});
            Emit(output, 0xb630u, {XForm(3, 26, 0, 467), XForm(4, 26, 0, 339)});
            Emit(output, 0xb640u, {XForm(3, 27, 0, 467), XForm(4, 27, 0, 339)});
            Emit(output, 0xb650u, {XForm(3, 16, 8, 467), XForm(4, 16, 8, 339)});
            for (unsigned opcode : {24u, 28u, 444u, 536u}) {
                Emit(output, 0x1000u + opcode * 4u,
                     {XForm(3, 4, 5, opcode, true)});
            }
            Emit(output, 0x3000, {0x34640001u});
            for (unsigned indexed = 0; indexed < 2; ++indexed) {
                for (unsigned index = 0; index < 32; ++index) {
                    const unsigned gqr = index / 4;
                    const bool single = (index & 2) != 0;
                    const bool update = (index & 1) != 0;
                    const uint32_t word = indexed
                        ? (4u << 26) | (3u << 21) | (4u << 16) | (5u << 11) |
                            (uint32_t(single) << 10) | (gqr << 7) | ((update ? 39u : 7u) << 1)
                        : ((update ? 61u : 60u) << 26) | (3u << 21) | (4u << 16) |
                            (uint32_t(single) << 15) | (gqr << 12) | 0xffcu;
                    Emit(output, 0xe000u + (indexed * 32 + index) * 16, {word});
                }
            }
            output << "static void (*const PairedStoreTestFunctions[])(PpcState&) = {\n";
            for (unsigned index = 0; index < 64; ++index) {
                output << "ppc_0000" << std::hex << std::uppercase << (0xe000u + index * 16)
                       << ",\n" << std::dec;
            }
            output << "};\n";
            Emit(output, 0x9300, {(31u << 26) | (3u << 21) | (83u << 1)});
            Emit(output, 0x9310, {(31u << 26) | (3u << 21) | (146u << 1)});
            Emit(output, 0x9320, {0x7c78e2a6u});
            Emit(output, 0x9330, {0x7c78e3a6u});
            Emit(output, 0x9340, {0x7c70faa6u});
            Emit(output, 0x9350, {0x7c70fba6u});
            Emit(output, 0x9360, {0x7c0004acu});
            Emit(output, 0x9370, {0x4c00012cu});
            Emit(output, 0x9380, {0x7c79faa6u});
            Emit(output, 0x9390, {0x7c79fba6u});
            Emit(output, 0xb100, {XForm(0, 0, 3, 86)});
            Emit(output, 0xb110, {XForm(0, 4, 3, 86)});
            Emit(output, 0xb120, {XForm(0, 0, 3, 982)});
            Emit(output, 0xb130, {XForm(0, 4, 3, 982)});
            Emit(output, 0xb200, {0x80640000u});
            Emit(output, 0xb210, {0x90640000u});
            Emit(output, 0xb220, {0xa0640000u});
            Emit(output, 0xb230, {0xb0640000u});
            for (unsigned record = 0; record < 2; ++record) {
                for (unsigned alias = 0; alias < 3; ++alias) {
                    Emit(output, 0xb300u + (record * 3 + alias) * 16u,
                         {XForm(3, alias == 0 ? 4 : alias == 1 ? 3 : 5, 5, 60, record != 0)});
                }
            }
            unsigned monitor = 0;
            for (unsigned spr : {952u, 956u, 953u, 954u, 957u, 958u}) {
                Emit(output, 0xb000u + monitor++ * 16u, {
                    XForm(3, spr & 31u, spr >> 5, 467)});
            }
            output << "static void (*const PerformanceResetTestFunctions[])(PpcState&) = {\n";
            for (unsigned i = 0; i < monitor; ++i) {
                output << "    ppc_0000" << std::hex << std::uppercase
                       << (0xb000u + i * 16u) << ",\n" << std::dec;
            }
            output << "};\n";
            for (unsigned extended = 0; extended < 2; ++extended) {
                for (unsigned record = 0; record < 2; ++record) {
                    for (unsigned alias = 0; alias < 3; ++alias) {
                        Emit(output, 0x9400u + (extended * 6 + record * 3 + alias) * 16u,
                            {XForm(alias == 0 ? 3 : alias == 1 ? 4 : 5, 4, 5,
                                   extended ? 138 : 10, record != 0)});
                    }
                }
            }
            output << "static void (*const AddCarryTestFunctions[])(PpcState&) = {\n";
            for (unsigned i = 0; i < 12; ++i) {
                output << "    ppc_0000" << std::hex << std::uppercase
                       << (0x9400u + i * 16u) << ",\n" << std::dec;
            }
            output << "};\n";
            Emit(output, 0x3010, {0x3464ffffu});
            Emit(output, 0x3020, {0x7c6903a6u, 0x7c8902a6u});
            Emit(output, 0x3030, {0x7c6103a6u, 0x7c8102a6u});
            Emit(output, 0x3040, {0x41820011u, 0x38630001u});
            Emit(output, 0x3050, {0xb8640000u});
            Emit(output, 0x3060, {XForm(3, 4, 5, 124, true)});
            Emit(output, 0x3070, {XForm(0, 3, 4, 0)});
            Emit(output, 0x3080, {XForm(12, 3, 4, 32)});
            Emit(output, 0x3090, {XForm(3, 4, 0, 26, true)});
            Emit(output, 0x30a0, {XForm(3, 3, 0, 26)});
            Emit(output, 0x30b0, {XForm(3, 4, 0, 922, true)});
            Emit(output, 0x30c0, {XForm(3, 3, 0, 922)});
            Emit(output, 0x30d0, {0x7c6c42e6u});
            Emit(output, 0x30e0, {0x7c8d42e6u});
            Emit(output, 0x30f0, {(19u << 26) | (6u << 21) | (7u << 16) |
                                  (8u << 11) | (193u << 1)});
            Emit(output, 0x3100, {0x4cc63182u});
            Emit(output, 0x3110, {0x20640001u});
            Emit(output, 0x3120, {0x2064ffffu});
            Emit(output, 0x3130, {0x20640000u});
            Emit(output, 0x3140, {XForm(3, 4, 5, 235, true)});
            Emit(output, 0x3150, {XForm(3, 4, 5, 459, true)});
            Emit(output, 0x3160, {XForm(3, 4, 5, 235, false)});
            Emit(output, 0x3170, {XForm(3, 4, 5, 459, false)});
            Emit(output, 0xf300, {XForm(3, 4, 5, 491, true)});
            Emit(output, 0xf310, {XForm(4, 4, 5, 491, false)});
            Emit(output, 0x3180, {XForm(3, 4, 0, 954, true)});
            Emit(output, 0x3190, {XForm(3, 3, 0, 954)});
            Emit(output, 0x31a0, {XForm(3, 4, 0, 104, true)});
            Emit(output, 0x31b0, {XForm(4, 4, 0, 104)});
            Emit(output, 0x31c0, {XForm(3, 4, 5, 136)});
            Emit(output, 0x31d0, {XForm(3, 4, 5, 136, true)});
            Emit(output, 0x31e0, {XForm(4, 4, 5, 136, true)});
            Emit(output, 0x31f0, {XForm(5, 4, 5, 136)});
            Emit(output, 0x4000, {0x7fe802a6u, 0x4800001du, 0x38630001u,
                                  0x7fe803a6u, 0x4e800020u});
            Emit(output, 0x4020, {0x38630002u, 0x4800001cu});
            Emit(output, 0x4040, {0x38630004u, 0x4e800020u});
            Emit(output, 0x4060, {0x7fe802a6u, 0x4e800421u,
                                  0x7fe803a6u, 0x4e800020u});
            Emit(output, 0x4080, {0x38600001u, 0x4e800420u, 0x38600063u,
                                  0x4e800020u, 0x38600007u, 0x4e800020u});
            Emit(output, 0x40a0, {0x4d820420u, 0x38600003u, 0x4e800020u,
                                  0x38600009u, 0x4e800020u});
            Emit(output, 0x40e0, {0x38630001u, 0x2c030003u, 0x4d800420u,
                                  0x4e800020u});
            unsigned transfer = 0;
            for (unsigned opcode : {50u, 51u, 54u, 55u}) {
                Emit(output, 0x5000u + transfer++ * 16u,
                    {(opcode << 26) | (3u << 21) | (3u << 16) | 0xfff8u});
            }
            for (unsigned opcode : {599u, 631u, 727u, 759u}) {
                Emit(output, 0x5000u + transfer++ * 16u, {XForm(3, 3, 4, opcode)});
            }
            Emit(output, 0x5080, {(50u << 26) | (3u << 21) | 0x100u});
            Emit(output, 0x5090, {(54u << 26) | (3u << 21) | 0x100u});
            Emit(output, 0x50a0, {XForm(3, 0, 4, 599)});
            Emit(output, 0x50b0, {XForm(3, 0, 4, 727)});
            Emit(output, 0x50c0, {XForm(3, 3, 3, 631)});
            Emit(output, 0x50d0, {XForm(3, 3, 3, 759)});
            unsigned singleTransfer = 0;
            for (unsigned opcode : {48u, 49u, 52u, 53u}) {
                Emit(output, 0xb700u + singleTransfer++ * 16u,
                    {(opcode << 26) | (3u << 21) | (3u << 16) | 0xfffcu});
            }
            Emit(output, 0xb740u, {(48u << 26) | (3u << 21) | 0x100u});
            Emit(output, 0xb750u, {(52u << 26) | (3u << 21) | 0x100u});
            unsigned unary = 0;
            for (unsigned opcode : {72u, 40u, 264u, 136u}) {
                for (bool record : {false, true}) {
                    for (unsigned source : {4u, 3u}) {
                        Emit(output, 0x5200u + unary++ * 16u,
                            {(63u << 26) | (3u << 21) | (source << 11) |
                             (opcode << 1) | static_cast<unsigned>(record)});
                    }
                }
            }
            std::vector<uint32_t> fpscrFunctions;
            for (uint32_t fieldMask = 0; fieldMask < 256; ++fieldMask) {
                for (uint32_t record = 0; record < 2; ++record) {
                    const uint32_t address = 0x6000u +
                        static_cast<uint32_t>(fpscrFunctions.size()) * 4u;
                    Emit(output, address, {(63u << 26) | (fieldMask << 17) |
                                          (4u << 11) | (711u << 1) | record});
                    fpscrFunctions.push_back(address);
                }
            }
            for (uint32_t field = 0; field < 8; ++field) {
                for (uint32_t value = 0; value < 16; ++value) {
                    for (uint32_t record = 0; record < 2; ++record) {
                        const uint32_t address = 0x6000u +
                            static_cast<uint32_t>(fpscrFunctions.size()) * 4u;
                        Emit(output, address, {(63u << 26) | (field << 23) |
                                              (value << 12) | (134u << 1) | record});
                        fpscrFunctions.push_back(address);
                    }
                }
            }
            output << "static void (*const FpscrTestFunctions[])(PpcState&) = {\n";
            for (uint32_t address : fpscrFunctions) {
                output << "    ppc_" << std::hex << std::uppercase;
                output.width(8);
                output.fill('0');
                output << address << ",\n" << std::dec;
            }
            output << "};\n";
            Emit(output, 0x7000u, {(63u << 26) | (3u << 21) | (583u << 1)});
            Emit(output, 0x7010u, {(63u << 26) | (3u << 21) | (583u << 1) | 1u});
            unsigned fpscrBit = 0;
            for (unsigned set = 0; set < 2; ++set) {
                for (unsigned bit = 0; bit < 32; ++bit) {
                    for (unsigned record = 0; record < 2; ++record) {
                        Emit(output, 0xa000u + fpscrBit++ * 4u, {
                            (63u << 26) | (bit << 21) |
                            ((set ? 38u : 70u) << 1) | record});
                    }
                }
            }
            output << "static void (*const FpscrBitTestFunctions[])(PpcState&) = {\n";
            for (unsigned i = 0; i < fpscrBit; ++i) {
                output << "    ppc_0000" << std::hex << std::uppercase
                       << (0xa000u + i * 4u) << ",\n" << std::dec;
            }
            output << "};\n";
            for (unsigned i = 0; i < 8; ++i) {
                Emit(output, 0x8000u + i * 16u, {
                    XForm(3, 16u + i, 28, 467), XForm(4, 16u + i, 28, 339)});
                for (unsigned single = 0; single < 2; ++single) {
                    for (unsigned update = 0; update < 2; ++update) {
                        Emit(output, 0x8100u + (i * 4u + single * 2u + update) * 16u, {
                            ((56u + update) << 26) | (3u << 21) | (3u << 16) |
                            (single << 15) | (i << 12) | 0xffcu});
                        Emit(output, 0x9000u + (i * 4u + single * 2u + update) * 16u, {
                            (4u << 26) | (3u << 21) | (3u << 16) | (4u << 11) |
                            (single << 10) | (i << 7) | ((6u + update * 32u) << 1)});
                    }
                }
            }
            Emit(output, 0x8400, {(56u << 26) | (3u << 21) | 0x100u});
            Emit(output, 0x9200, {(4u << 26) | (3u << 21) | (4u << 11) | (6u << 1)});
            Emit(output, 0x9210, {(4u << 26) | (3u << 21) | (3u << 16) |
                                 (3u << 11) | (38u << 1)});
            Emit(output, 0x9220, {(4u << 26) | (3u << 21) | (3u << 16) | (6u << 1)});
            unsigned paired = 0;
            for (unsigned opcode : {72u, 40u, 264u, 136u, 528u, 560u, 592u, 624u}) {
                for (unsigned record = 0; record < 2; ++record) {
                    for (unsigned alias = 0; alias < 3; ++alias) {
                        const unsigned left = alias == 1 ? 3 : 4;
                        const unsigned right = alias == 2 ? 3 : 5;
                        Emit(output, 0x8500u + paired++ * 16u, {
                            (4u << 26) | (3u << 21) | ((opcode >= 528 ? left : 0u) << 16) |
                            (right << 11) | (opcode << 1) | record});
                    }
                }
            }
            output << "static void (*const PairedLoadTestFunctions[])(PpcState&) = {\n";
            for (unsigned i = 0; i < 32; ++i) {
                output << "    ppc_0000" << std::hex << std::uppercase
                       << (0x8100u + i * 16u) << ",\n" << std::dec;
            }
            output << "};\nstatic void (*const IndexedPairedLoadTestFunctions[])(PpcState&) = {\n";
            for (unsigned i = 0; i < 32; ++i) {
                output << "    ppc_0000" << std::hex << std::uppercase
                       << (0x9000u + i * 16u) << ",\n" << std::dec;
            }
            output << "};\nstatic void (*const PairedMoveTestFunctions[])(PpcState&) = {\n";
            for (unsigned i = 0; i < paired; ++i) {
                output << "    ppc_0000" << std::hex << std::uppercase
                       << (0x8500u + i * 16u) << ",\n" << std::dec;
            }
            output << "};\n";
            std::vector<uint16_t> dsp = {
                0x029f, 2, 0x1200,
                0x0080, 0x8000, 0x0088, 0xffff, 0x0084, 4
            };
            const auto romLoop = [&](uint16_t load) {
                const auto pc = dsp.size();
                dsp.insert(dsp.end(), {0x0064, static_cast<uint16_t>(pc + 3), load, 0});
            };
            romLoop(0x0218);
            dsp.insert(dsp.end(), {0x8100, 0x1c1e, 0x0044, 0x1b1e,
                                   0x0080, 0x1000, 0x0084, 3});
            romLoop(0x191e);
            const auto readPc = dsp.size();
            dsp.insert(dsp.end(), {0x00de, 0xfffc, 0x02a0, 0x8000,
                                   0x029c, static_cast<uint16_t>(readPc),
                                   0x16fc, 0x1234, 0x16fd, 0x5678, 0x0021});
            output << "#include \"native_dsp_init.h\"\n";
            output << RecompileDspInitialization(dsp, "SyntheticDspInitialization");
            output << "static const std::vector<uint16_t> SyntheticDspWords = {";
            for (auto word : dsp) output << word << "u,";
            output << "};\n";
            dsp[dsp.size() - 4] = 0x2345;
            dsp[dsp.size() - 2] = 0x6789;
            output << RecompileDspInitialization(dsp, "OtherSyntheticDspInitialization");
            dsp[8] = 0x1001;
            output << RecompileDspInitialization(dsp, "OutOfRangeDspInitialization");
            dsp[8] = 4;
            dsp[6] = 0xfffe;
            output << RecompileDspInitialization(dsp, "WrappedDspInitialization");
            Emit(output, 0xb5e0u, {
                (31u << 26) | (3u << 21) | (26u << 16) | (467u << 1),
                (31u << 26) | (4u << 21) | (27u << 16) | (467u << 1)});
            Emit(output, 0xb5f0u, {
                (31u << 26) | (3u << 21) | (16u << 16) | (16u << 11) | (467u << 1)});
            generated += output.str();
        }
        std::ofstream output(argv[argc - 1], std::ios::binary);
        output << generated;
        output.close();
        if (!output) {
            throw std::runtime_error("Could not write generated test C++.");
        }
    } catch (const std::exception& error) {
        std::cerr << "Test generation error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
