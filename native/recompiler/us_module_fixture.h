#ifndef HMAWL_US_MODULE_FIXTURE_H
#define HMAWL_US_MODULE_FIXTURE_H

#include "disc_import.h"
#include "dol_image.h"
#include "rel_image.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>

struct UsModuleFixture {
    PpcState state;
    std::vector<RelImage> images;
    std::vector<LinkedRelModule> modules;
};

inline UsModuleFixture LoadUsModuleFixture(const std::filesystem::path& directory) {
    std::vector<DiscFile> files;
    for (const char* name : {"main.dol", "wlp0.rel", "wlp1.rel", "wlp2.rel"}) {
        std::ifstream input(directory / name, std::ios::binary);
        if (!input) {
            throw std::runtime_error(std::string("Could not open US test input: ") + name);
        }
        files.push_back({name, std::vector<uint8_t>(
            std::istreambuf_iterator<char>(input), {})});
        if (input.bad()) {
            throw std::runtime_error(std::string("Could not read US test input: ") + name);
        }
    }
    VerifyUsExecutables(files);
    const auto dol = DolImage::Parse(std::move(files[0].bytes));
    UsModuleFixture fixture;
    fixture.state = dol.CreateGameCubeState();
    std::vector<RelMemoryRange> reserved = {
        {0x80000000u, 0x3100}, {0x8034cfa0u, 0x10000}};
    for (const auto& section : dol.sections()) {
        reserved.push_back({section.address, section.size});
    }
    reserved.push_back({dol.bssAddress(), dol.bssSize()});
    for (size_t i = 1; i < files.size(); ++i) {
        fixture.images.push_back(RelImage::Parse(std::move(files[i].bytes)));
    }
    fixture.modules = LinkRelModules(fixture.state, {
        {&fixture.images[0], 0x81000000u},
        {&fixture.images[1], 0x81200000u},
        {&fixture.images[2], 0x81400000u}}, reserved);
    return fixture;
}

#endif
