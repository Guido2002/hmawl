#ifndef HMAWL_DISC_IMPORT_H
#define HMAWL_DISC_IMPORT_H

#include "rvz_image.h"

#include <filesystem>
#include <string>
#include <vector>

struct DiscFile {
    std::string name;
    std::vector<uint8_t> bytes;
};

struct DiscFileEntry {
    std::string path;
    uint32_t offset;
    uint32_t size;
};

std::vector<DiscFileEntry> ReadGameCubeFileTable(RvzImage& image);
DiscFile ReadGameCubeFile(RvzImage& image, const std::string& path);
std::vector<DiscFile> ReadGameCubeExecutables(RvzImage& image);
void VerifyUsExecutables(const std::vector<DiscFile>& files);
std::vector<DiscFile> ImportUsRvz(const std::filesystem::path& input,
                                 const std::filesystem::path& outputDirectory);

#endif
