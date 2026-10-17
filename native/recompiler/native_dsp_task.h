#ifndef HMAWL_NATIVE_DSP_TASK_H
#define HMAWL_NATIVE_DSP_TASK_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct PpcState;

enum class NativeDspTaskMode { Inactive, LoaderCommand, LoaderValue, Running, Halted };

struct NativeDspTask {
    NativeDspTaskMode mode = NativeDspTaskMode::Inactive;
    uint16_t command = 0;
    uint16_t parameters = 0;
    uint32_t source = 0;
    uint16_t destination = 0;
    uint16_t bytes = 0;
    uint16_t dataBytes = 0;
    uint16_t entry = 0;
    std::array<uint16_t, 4096> instructionRam = {};
    std::array<uint16_t, 32> registers = {};
    uint32_t loaderStarts = 0;
    uint32_t instructionUploads = 0;
    uint32_t uploadedBytes = 0;
    uint32_t cpuMailSent = 0;
    uint32_t cpuMailConsumed = 0;
    uint32_t interrupts = 0;
};

void NativeDspStartTaskLoader(PpcState& state);
void NativeDspWriteCpuMail(PpcState& state, uint32_t address, uint16_t value);
uint16_t NativeDspTaskRead(PpcState& state, uint16_t address);
void NativeDspTaskWrite(PpcState& state, uint16_t address, uint16_t value);
void NativeDspTaskWriteRegister(PpcState& state, unsigned reg, uint16_t value);
uint16_t NativeDspTaskReadRegister(const PpcState& state, unsigned reg);
void NativeDspAdvanceTask(PpcState& state);
std::string RecompileDspTask(const std::vector<uint16_t>& words, uint16_t entry,
                             const std::string& functionName);

#endif
