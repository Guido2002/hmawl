#include "dol_image.h"
#include "ppc_recompiler.h"
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
          const std::vector<uint32_t>& instructions) {
    output << RecompileFunction(address, instructions) << '\n';
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
                {0x80003154u, 4}, {0x800032b0u, 0x90}, {0x80003100u, 0x40},
                {0x80003140u, 12}, {0x8000314cu, 8}, {0x800031f8u, 0x68},
                {0x80003488u, 0x24}, {0x80003340u, 0xc0},
                {0x800054e4u, 0x30}, {0x80005514u, 0xb8},
                {0x800055ccu, 0x50}}) {
                generated += RecompileFunction(range.first,
                    image.ReadWords(range.first, range.second)) + '\n';
            }
        } else {
            std::ostringstream output;
            for (unsigned opcode : {24u, 28u, 444u, 536u}) {
                Emit(output, 0x1000u + opcode * 4u,
                     {XForm(3, 4, 5, opcode, true)});
            }
            Emit(output, 0x3000, {0x34640001u});
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
