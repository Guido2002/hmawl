#include "dol_image.h"
#include "disc_import.h"
#include "clz_image.h"
#include "ppc_recompiler.h"
#include "rvz_image.h"
#include "rel_image.h"
#include "sha1.h"

#include <charconv>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

uint32_t ParseNumber(const char* value, const char* name) {
    const std::string text(value);
    const char* begin = text.data();
    int base = 10;
    if (text.size() > 2 && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        begin += 2;
        base = 16;
    }
    uint32_t result = 0;
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(begin, end, result, base);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error(std::string("Invalid ") + name + ": " + text);
    }
    return result;
}

} // namespace

int main(int argc, char** argv) {
    if ((argc == 3 && std::string(argv[1]) == "--list-rvz-files") ||
        (argc == 5 && (std::string(argv[1]) == "--extract-rvz-file" ||
                       std::string(argv[1]) == "--extract-rvz-clz"))) {
        try {
            RvzImage image(argv[2]);
            if (argc == 3) {
                for (const auto& entry : ReadGameCubeFileTable(image)) {
                    std::cout << entry.path << '\t' << entry.size << '\t'
                              << entry.offset << '\n';
                }
            } else {
                if (std::filesystem::exists(argv[4])) {
                    throw std::runtime_error("Output file already exists; choose a new path.");
                }
                auto file = ReadGameCubeFile(image, argv[3]);
                if (std::string(argv[1]) == "--extract-rvz-clz") {
                    file.bytes = DecodeClz(file.bytes, 24u * 1024u * 1024u);
                }
                std::ofstream output(argv[4], std::ios::binary);
                if (!output) {
                    throw std::runtime_error("Could not create extracted disc file.");
                }
                output.write(reinterpret_cast<const char*>(file.bytes.data()),
                             static_cast<std::streamsize>(file.bytes.size()));
                output.close();
                if (!output) {
                    throw std::runtime_error("Could not finish writing extracted disc file.");
                }
                std::cout << file.name << ": " << file.bytes.size()
                          << " bytes, SHA-1 " << Sha1Hex(file.bytes) << '\n';
            }
        } catch (const std::exception& error) {
            std::cerr << "Disc file error: " << error.what() << '\n';
            return 1;
        }
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--inspect-rel") {
        try {
            std::ifstream input(argv[2], std::ios::binary);
            if (!input) {
                throw std::runtime_error("Could not open input REL.");
            }
            const auto image = RelImage::Parse(
                std::vector<uint8_t>((std::istreambuf_iterator<char>(input)), {}));
            std::cout << "Module ID: " << image.id() << "\nREL version: 3\n"
                      << "Sections: " << image.sections().size()
                      << "\nModule alignment: " << image.alignment()
                      << "\nBSS alignment: " << image.bssAlignment()
                      << "\nRelocation patches: " << image.relocations().size()
                      << "\nImports:";
            for (uint32_t id : image.imports()) {
                std::cout << ' ' << id;
            }
            std::cout << '\n';
            for (size_t index = 0; index < image.sections().size(); ++index) {
                const auto& section = image.sections()[index];
                if (section.size != 0) {
                    std::cout << "Section " << index << ": file 0x" << std::hex
                              << section.fileOffset << ", size 0x" << section.size
                              << std::dec << (section.executable ? ", executable" :
                                  section.fileOffset == 0 ? ", BSS" : ", data") << '\n';
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "REL inspection error: " << error.what() << '\n';
            return 1;
        }
        return 0;
    }
    if (argc == 4 && std::string(argv[1]) == "--extract-rvz") {
        try {
            const auto files = ImportUsRvz(argv[2], argv[3]);
            for (const auto& file : files) {
                std::cout << file.name << ": " << file.bytes.size()
                          << " bytes, SHA-1 " << Sha1Hex(file.bytes) << '\n';
            }
            std::cout << "Verified US revision 0 executables extracted to "
                      << argv[3] << ".\n";
        } catch (const std::exception& error) {
            std::cerr << "RVZ extraction error: " << error.what() << '\n';
            return 1;
        }
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--inspect-rvz") {
        try {
            const RvzMetadata metadata = InspectRvz(argv[2]);
            std::cout << "Disc ID: " << metadata.discId << '\n'
                      << "Disc number: "
                      << static_cast<unsigned>(metadata.discNumber) << '\n'
                      << "Disc revision: "
                      << static_cast<unsigned>(metadata.discVersion) << '\n'
                      << "Title: " << metadata.title << '\n'
                      << "Image size: " << metadata.discSize << " bytes\n"
                      << "RVZ chunk size: " << metadata.chunkSize << " bytes\n"
                      << "RVZ compression: " << metadata.compression << '\n'
                      << "Raw data records: " << metadata.rawGroupCount << '\n'
                      << "Data groups: " << metadata.compressedGroupCount << '\n';
        } catch (const std::exception& error) {
            std::cerr << "RVZ inspection error: " << error.what() << '\n';
            return 1;
        }
        return 0;
    }

    if (argc != 5) {
        std::cerr << "Usage:\n"
                     "  hmawl_recompiler --inspect-rvz <input.rvz>\n"
                     "  hmawl_recompiler --inspect-rel <input.rel>\n"
                     "  hmawl_recompiler --extract-rvz <input.rvz> <new-output-directory>\n"
                     "  hmawl_recompiler --list-rvz-files <input.rvz>\n"
                     "  hmawl_recompiler --extract-rvz-file <input.rvz> <disc/path> <new-output-file>\n"
                     "  hmawl_recompiler --extract-rvz-clz <input.rvz> <disc/path> <new-output-file>\n"
                     "  hmawl_recompiler <input.dol> <address> <size> "
                     "<output.cpp>\n"
                     "Translates a 4-byte-aligned DOL text range to C++.\n";
        return 2;
    }

    try {
        std::ifstream input(argv[1], std::ios::binary);
        if (!input) {
            throw std::runtime_error(std::string("Could not open input DOL: ") +
                                     argv[1]);
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)),
                                   std::istreambuf_iterator<char>());
        const DolImage image = DolImage::Parse(std::move(bytes));
        const uint32_t address = ParseNumber(argv[2], "code address");
        const uint32_t size = ParseNumber(argv[3], "code size");
        const std::vector<uint32_t> words = image.ReadWords(address, size);
        const std::string generated = RecompileFunction(address, words);

        std::ofstream output(argv[4], std::ios::binary);
        if (!output) {
            throw std::runtime_error(std::string("Could not open output file: ") +
                                     argv[4]);
        }
        output << "#include \"ppc_state.h\"\n\n" << generated;
        if (!output) {
            throw std::runtime_error("Could not write generated C++ output.");
        }
        std::cout << "Translated " << words.size() << " instructions at 0x"
                  << std::hex << address << " to " << argv[4] << ".\n";
    } catch (const std::exception& error) {
        std::cerr << "Recompiler error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
