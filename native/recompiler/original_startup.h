#ifndef HMAWL_ORIGINAL_STARTUP_H
#define HMAWL_ORIGINAL_STARTUP_H

#include "dol_image.h"
#include "native_dispatch.h"

#include <filesystem>
#include <string>

enum class OriginalStartupStop { Dependency, Budget, UnexpectedReturn };

struct OriginalStartupResult {
    OriginalStartupStop stop;
    uint32_t pc;
    uint32_t lr;
    uint32_t stack;
    uint32_t argument3;
    uint32_t argument4;
    uint32_t argument5;
    std::string reason;
};

DolImage LoadOriginalUsDol(const std::filesystem::path& path);
PpcState CreateOriginalStartupState(const DolImage& image);
void RegisterOriginalStartup(NativeDispatcher& dispatcher, const PpcState& state);
OriginalStartupResult RunOriginalStartup(NativeDispatcher& dispatcher, PpcState& state,
                                        size_t blockBudget = 1000000,
                                        size_t maxCallDepth = 128);
std::string DescribeOriginalStartupStop(const OriginalStartupResult& result);

#endif
