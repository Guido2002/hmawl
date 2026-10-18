#include "startup_diagnostics.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace {
std::string Quote(const std::string& value) {
    std::ostringstream out;
    out << '"';
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c < 0x20) out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c);
        else out << c;
    }
    out << '"';
    return out.str();
}
std::string Hex(uint32_t word) {
    std::ostringstream out;
    out << "0x" << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << word;
    return Quote(out.str());
}
// Deliberately bypass all hardware register read handlers (some consume data).
std::optional<uint32_t> RamWord(const PpcState& state, uint32_t address) {
    if (address & 3u) return {};
    try {
        const auto offset = ppc_memory_offset(state, address, 4);
        return (uint32_t(state.memory[offset]) << 24) | (uint32_t(state.memory[offset + 1]) << 16) |
               (uint32_t(state.memory[offset + 2]) << 8) | state.memory[offset + 3];
    } catch (const std::exception&) { return {}; }
}
std::string OptionalWord(std::optional<uint32_t> word) { return word ? Hex(*word) : "null"; }
}

std::string ClassifyStartupStop(const OriginalStartupResult& result) {
    if (result.stop == OriginalStartupStop::Budget) return "execution_limit";
    if (result.stop == OriginalStartupStop::UnexpectedReturn) return "unexpected_entry_return";
    const auto& reason = result.reason;
    if (reason.find("No native translation registered") != std::string::npos) return "missing_translation";
    if (reason.find("code mismatch") != std::string::npos) return "code_integrity";
    if (reason.find("PowerPC") != std::string::npos) return "powerpc_semantics";
    if (reason.find("GX") != std::string::npos || reason.find("EFB") != std::string::npos) return "graphics";
    if (reason.find("DVD") != std::string::npos || reason.find("RVZ") != std::string::npos) return "disc_io";
    return "unclassified_dependency";
}

StartupDiagnostics::StartupDiagnostics(size_t historyCapacity, size_t instructionCapacity) {
    if (historyCapacity > 65536) throw std::runtime_error("Block history is limited to 65536 entries.");
    if (instructionCapacity > 65536) throw std::runtime_error("Instruction history is limited to 65536 entries.");
    history_.resize(historyCapacity);
    instructionHistory_.resize(instructionCapacity);
    reads_.reserve(64);
}
void StartupDiagnostics::ObserveBlock(uint32_t pc) {
    ++blocks_;
    if (history_.empty()) return;
    history_[next_] = pc;
    next_ = (next_ + 1) % history_.size();
    count_ = std::min(count_ + 1, history_.size());
}
void StartupDiagnostics::ObserveInstruction(uint32_t pc, uint32_t instruction) {
    if (instructionHistory_.empty()) return;
    ++instructions_;
    instructionHistory_[instructionNext_] = {pc, instruction};
    instructionNext_ = (instructionNext_ + 1) % instructionHistory_.size();
    instructionCount_ = std::min(instructionCount_ + 1, instructionHistory_.size());
}
void StartupDiagnostics::SetFiles(std::vector<DiscFileEntry> files) { files_ = std::move(files); }
void StartupDiagnostics::ObserveDisc(const PpcState& state, const std::array<uint32_t, 3>& command,
                                     uint32_t length, bool completed, const std::string& error) {
    DiscEvent event{command[0], state.discInterface ? state.discInterface->dmaAddress : 0,
                    length, uint64_t(command[1]) * 4, completed, error, {}};
    if (command[0] == 0xa8000000u) {
        for (const auto& file : files_) {
            if (event.offset >= file.offset && event.offset < uint64_t(file.offset) + file.size) {
                event.path = file.path;
                break;
            }
        }
    }
    if (reads_.size() == 64) reads_.erase(reads_.begin());
    reads_.push_back(std::move(event));
}
void StartupDiagnostics::ObserveDecoder(const PpcState& state) {
    decoderPath_.clear();
    // Last successful transfer covering the compressed source; aliases share physical RAM.
    const uint32_t source = state.gpr[4] & 0x3fffffffu;
    for (auto it = reads_.rbegin(); it != reads_.rend(); ++it) {
        const uint32_t destination = it->destination & 0x3fffffffu;
        if (it->completed && it->command == 0xa8000000u && source >= destination &&
            uint64_t(source) < uint64_t(destination) + it->length) {
            decoderPath_ = it->path;
            break;
        }
    }
    if (decoders_.size() == 16) decoders_.erase(decoders_.begin());
    decoders_.push_back({state.gpr[4], state.gpr[3],
        state.gpr[4] <= UINT32_MAX - 4 ? RamWord(state, state.gpr[4] + 4) : std::nullopt, decoderPath_});
}

std::string StartupDiagnostics::Json(const PpcState& state, const OriginalStartupResult& result,
                                     size_t budget, size_t depth) const {
    std::ostringstream out;
    const auto category = ClassifyStartupStop(result);
    out << "{\n  \"schema_version\": 1,\n  \"classification\": " << Quote(category)
        << ",\n  \"classification_basis\": \"stop kind and error-message routing; not a root-cause diagnosis\","
        << "\n  \"failure_key\": " << Quote(category + ":" + std::to_string(result.pc) + ":" + result.reason)
        << ",\n  \"reason\": " << Quote(result.reason)
        << ",\n  \"exit_code\": " << (result.stop == OriginalStartupStop::Budget ? 4 :
                                        result.stop == OriginalStartupStop::UnexpectedReturn ? 5 : 3)
        << ",\n  \"limits\": {\"blocks\": " << budget << ", \"call_depth\": " << depth << "},"
        << "\n  \"observed_block_entries\": " << blocks_
        << ",\n  \"cpu\": {\"pc\": " << Hex(state.pc) << ", \"instruction\": " << OptionalWord(RamWord(state, state.pc))
        << ", \"lr\": " << Hex(state.lr) << ", \"ctr\": " << Hex(state.ctr)
        << ", \"cr\": " << Hex(state.cr) << ", \"xer\": " << Hex(state.xer)
        << ", \"msr\": " << Hex(ppc_read_msr(state)) << ", \"fpscr\": " << Hex(state.fpscr)
        << ", \"srr0\": " << Hex(state.srr0) << ", \"srr1\": " << Hex(state.srr1) << ", \"gpr\": [";
    for (unsigned i = 0; i < 32; ++i) out << (i ? ", " : "") << Hex(state.gpr[i]);
    out << "], \"fpr_bits\": [";
    for (unsigned i = 0; i < 32; ++i) {
        if (i) out << ", ";
        out << '[' << Quote(std::to_string(state.fpr[i].ps0)) << ',' << Quote(std::to_string(state.fpr[i].ps1)) << ']';
    }
    out << "]},\n  \"active_thread\": " << OptionalWord(RamWord(state, 0x800000e4u))
        << ",\n  \"stack_backchain\": [";
    uint32_t stack = state.gpr[1];
    for (unsigned frame = 0; frame < 32; ++frame) {
        if (stack > UINT32_MAX - 4) break;
        const auto next = RamWord(state, stack), saved = RamWord(state, stack + 4);
        if (!next || !saved) break;
        if (frame) out << ',';
        out << "{\"sp\":" << Hex(stack) << ",\"backchain\":" << Hex(*next)
            << ",\"saved_lr_slot\":" << Hex(*saved) << '}';
        if (*next <= stack || (*next & 3)) break;
        stack = *next;
    }
    out << "],\n  \"history_kind\": \"recent_blocks are compiled block entries; recent_instructions are generated instruction entries when enabled\","
        << "\n  \"recent_blocks\": [";
    for (size_t i = 0; i < count_; ++i) {
        const auto pc = history_[(next_ + history_.size() - count_ + i) % history_.size()];
        if (i) out << ',';
        out << Hex(pc);
    }
    out << "],\n  \"instruction_trace_enabled\": " << (instructionHistory_.empty() ? "false" : "true")
        << ",\n  \"observed_instruction_entries\": " << instructions_
        << ",\n  \"recent_instructions\": [";
    for (size_t i = 0; i < instructionCount_; ++i) {
        const auto& instruction = instructionHistory_[
            (instructionNext_ + instructionHistory_.size() - instructionCount_ + i) % instructionHistory_.size()];
        if (i) out << ',';
        out << "{\"pc\":" << Hex(instruction.pc) << ",\"word\":" << Hex(instruction.instruction) << '}';
    }
    out << "],\n  \"decoder\": {\"resource_path\": " << (decoderPath_.empty() ? "null" : Quote(decoderPath_))
        << ", \"source\": " << Hex(result.clzSource) << ", \"destination\": " << Hex(result.clzDestination)
        << ", \"expected_output_bytes\": " << result.clzExpectedBytes
        << ", \"produced_output_bytes\": " << result.clzProducedBytes
        << ", \"block_entries\": " << result.clzDecoderBlockEntries
        << ", \"starts\": " << result.clzDecodesStarted << ", \"returns\": " << result.clzDecodesCompleted << "},"
        << "\n  \"recent_decoder_starts\": [";
    for (size_t i = 0; i < decoders_.size(); ++i) {
        const auto& decoder = decoders_[i];
        if (i) out << ',';
        out << "{\"source\":" << Hex(decoder.source) << ",\"destination\":" << Hex(decoder.destination)
            << ",\"expected_output_bytes\":" << (decoder.expected ? std::to_string(*decoder.expected) : "null")
            << ",\"path\":" << (decoder.path.empty() ? "null" : Quote(decoder.path)) << '}';
    }
    out << "],\n  \"game_loop_iterations\": " << result.gameLoopIterationsCompleted
        << ",\n  \"recent_disc_transfers\": [";
    for (size_t i = 0; i < reads_.size(); ++i) {
        const auto& read = reads_[i];
        if (i) out << ',';
        out << "{\"command\":" << Hex(read.command) << ",\"disc_offset\":" << read.offset
            << ",\"ram_destination\":" << Hex(read.destination) << ",\"length\":" << read.length
            << ",\"backend_returned\":" << (read.completed ? "true" : "false")
            << ",\"path\":" << (read.path.empty() ? "null" : Quote(read.path)) << ",\"error\":" << Quote(read.error) << '}';
    }
    out << "],\n  \"disc_interface\": ";
    if (state.discInterface) {
        const auto& di = *state.discInterface;
        out << "{\"status\":" << Hex(di.status) << ",\"control\":" << Hex(di.control)
            << ",\"dma_address\":" << Hex(di.dmaAddress) << ",\"dma_remaining\":" << di.dmaLength
            << ",\"completed_transfers\":" << di.completedTransfers << '}';
    } else out << "null";
    out << ",\n  \"graphics\": ";
    if (state.graphics) {
        const auto& gx = *state.graphics;
        out << "{\"draws\":" << gx.draws << ",\"pixel_writes\":" << gx.rasterizedPixels
            << ",\"pending_command_bytes\":" << gx.commands.size()
            << ",\"xfb_copies\":" << (gx.efb ? gx.efb->copies : 0) << '}';
    } else out << "null";
    out << ",\n  \"unavailable\": [\"native source location\",\"complete original call stack (backchain slots are unvalidated)\","
           "\"allocation history\",\"resource publication status\"";
    if (instructionHistory_.empty()) out << ",\"per-instruction history\"";
    out << "],\n  \"hypotheses\": []\n}\n";
    return out.str();
}

std::filesystem::path StartupDiagnostics::Write(const std::filesystem::path& directory,
                                                const PpcState& state, const OriginalStartupResult& result,
                                                size_t budget, size_t depth) const {
    std::filesystem::create_directories(directory);
    const auto stamp = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    std::filesystem::path run;
    for (unsigned suffix = 0; ; ++suffix) {
        run = directory / ("startup-" + std::to_string(stamp) + "-" + std::to_string(suffix));
        if (std::filesystem::create_directory(run)) break;
        if (suffix == 1000) throw std::runtime_error("Could not create a unique diagnostics directory.");
    }
    const auto write = [&](const char* name, const std::string& text) {
        std::ofstream file(run / name, std::ios::binary);
        file << text;
        file.close();
        if (!file) throw std::runtime_error("Could not write startup diagnostics.");
    };
    write("report.json", Json(state, result, budget, depth));
    write("summary.txt", "Classification: " + ClassifyStartupStop(result) + "\n" + DescribeOriginalStartupStop(result) +
          "\nSee report.json for evidence and explicit capture limitations.\n");
    return run;
}
