#ifndef HMAWL_DSP_INIT_RECOMPILER_H
#define HMAWL_DSP_INIT_RECOMPILER_H

#include <cstdint>
#include <string>
#include <vector>

// This bounded compiler accepts initialization microcode, not general DSP tasks.
std::string RecompileDspInitialization(const std::vector<uint16_t>& words,
                                      const std::string& functionName);

#endif
