#ifndef HMAWL_PPC_RECOMPILER_H
#define HMAWL_PPC_RECOMPILER_H

#include <cstdint>
#include <string>
#include <vector>

struct RecompileOptions {
    bool yieldAtBranches = false;
    bool precisePc = false;
};

std::string RecompileFunction(uint32_t address,
                              const std::vector<uint32_t>& instructions,
                              RecompileOptions options = {});

#endif
