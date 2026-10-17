#ifndef HMAWL_NATIVE_DSP_TASK_H
#define HMAWL_NATIVE_DSP_TASK_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

struct PpcState;

enum class NativeDspTaskMode { Inactive, LoaderCommand, LoaderValue, Running, Halted };

struct NativeDspTask {
    struct Loop {
        uint16_t start, end, remaining;
    };
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
    uint16_t dmaControl = 0;
    uint16_t dmaAddress = 0;
    uint32_t dmaMainAddress = 0;
    uint32_t dataTransfers = 0;
    uint64_t transferredBytes = 0;
    std::vector<uint16_t> returnAddresses;
    std::vector<Loop> loops;
};

void NativeDspStartTaskLoader(PpcState& state);
void NativeDspWriteCpuMail(PpcState& state, uint32_t address, uint16_t value);
uint16_t NativeDspTaskRead(PpcState& state, uint16_t address);
void NativeDspTaskWrite(PpcState& state, uint16_t address, uint16_t value);
void NativeDspTaskWriteRegister(PpcState& state, unsigned reg, uint16_t value);
uint16_t NativeDspTaskReadRegister(const PpcState& state, unsigned reg);
void NativeDspAdvanceTask(PpcState& state);
int64_t NativeDspAccumulator(const PpcState& state, unsigned acc);
void NativeDspSetAccumulator(PpcState& state, unsigned acc, int64_t value);
void NativeDspArithmeticFlags(PpcState& state, int64_t left, int64_t right, bool subtract);
void NativeDspLogicFlags(PpcState& state, unsigned acc);
bool NativeDspCondition(const PpcState& state, unsigned condition);
void NativeDspCall(PpcState& state, uint16_t resume);
uint16_t NativeDspReturn(PpcState& state);
void NativeDspStartLoop(PpcState& state, uint16_t start, uint16_t end, uint16_t count, uint16_t after);
uint16_t NativeDspNext(PpcState& state, uint16_t pc, uint16_t next);
std::string RecompileDspTask(const std::vector<uint16_t>& words, uint16_t entry,
                             const std::string& functionName,
                             const std::vector<uint16_t>& indirectEntries = {});

#endif
