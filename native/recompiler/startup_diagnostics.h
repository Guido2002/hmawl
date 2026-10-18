#ifndef HMAWL_STARTUP_DIAGNOSTICS_H
#define HMAWL_STARTUP_DIAGNOSTICS_H

#include "original_startup.h"
#include "disc_import.h"

// Bounded observation only: never reads MMIO, advances clocks, or changes guest state.
class StartupDiagnostics {
public:
    explicit StartupDiagnostics(size_t historyCapacity = 128, size_t instructionCapacity = 0);
    void ObserveBlock(uint32_t pc);
    void ObserveInstruction(uint32_t pc, uint32_t instruction);
    void ObserveDisc(const PpcState& state, const std::array<uint32_t, 3>& command,
                     uint32_t length, bool completed, const std::string& error = {});
    void ObserveDecoder(const PpcState& state);
    void SetFiles(std::vector<DiscFileEntry> files);
    std::string Json(const PpcState& state, const OriginalStartupResult& result,
                     size_t blockBudget, size_t maxDepth) const;
    std::filesystem::path Write(const std::filesystem::path& directory,
                                const PpcState& state, const OriginalStartupResult& result,
                                size_t blockBudget, size_t maxDepth) const;
private:
    struct DiscEvent {
        uint32_t command, destination, length;
        uint64_t offset;
        bool completed;
        std::string error, path;
    };
    std::vector<uint32_t> history_;
    size_t next_ = 0, count_ = 0;
    uint64_t blocks_ = 0;
    struct InstructionEvent { uint32_t pc, instruction; };
    std::vector<InstructionEvent> instructionHistory_;
    size_t instructionNext_ = 0, instructionCount_ = 0;
    uint64_t instructions_ = 0;
    std::vector<DiscEvent> reads_;
    std::vector<DiscFileEntry> files_;
    std::string decoderPath_;
    struct DecoderEvent { uint32_t source, destination; std::optional<uint32_t> expected; std::string path; };
    std::vector<DecoderEvent> decoders_;
};

std::string ClassifyStartupStop(const OriginalStartupResult& result);

#endif
