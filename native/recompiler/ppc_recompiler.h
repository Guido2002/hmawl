#ifndef HMAWL_PPC_RECOMPILER_H
#define HMAWL_PPC_RECOMPILER_H

#include <cstdint>
#include <string>
#include <vector>

std::string RecompileFunction(uint32_t address,
                              const std::vector<uint32_t>& instructions);

#endif
