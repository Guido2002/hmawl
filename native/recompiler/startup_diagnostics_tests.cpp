#include "startup_diagnostics.h"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void RecordInstruction(void* context, uint32_t pc, uint32_t instruction) {
    static_cast<std::vector<std::pair<uint32_t, uint32_t>>*>(context)->emplace_back(pc, instruction);
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected original DOL path.");
        const auto image = LoadOriginalUsDol(argv[1]);
        auto state = std::make_unique<PpcState>(CreateOriginalStartupState(image));
        NativeDispatcher dispatcher;
        RegisterOriginalStartup(dispatcher, *state);
        std::vector<std::pair<uint32_t, uint32_t>> executedInstructions;
        state->instructionObserverContext = &executedInstructions;
        state->instructionObserver = &RecordInstruction;
        state->gpr[3] = 0xffffffffu;
        state->lr = 0x80003000u;
        dispatcher.Execute(*state, 0x80154d00u);
        Require(state->gpr[3] == 0 && state->pc == 0x80003000u,
                "Historical stop function did not return zero through LR.");
        Require(executedInstructions == std::vector<std::pair<uint32_t, uint32_t>>{
                    {0x80154d00u, 0x38600000u}, {0x80154d04u, 0x4e800020u}},
                "Generated instruction observer did not report the executed PC/opcode sequence.");
        state->instructionObserver = nullptr;
        state->instructionObserverContext = nullptr;
        state->gpr[3] = 42;
        dispatcher.Execute(*state, 0x80154fb4u);
        Require(state->gpr[3] == 0 && state->pc == 0x80003000u,
                "Original adjustment thunk failed to tail-call historical stop function.");

        StartupDiagnostics diagnostics(3);
        for (uint32_t pc = 0x100; pc <= 0x110; pc += 4) diagnostics.ObserveBlock(pc);
        diagnostics.SetFiles({{"test\"file.clz", 0x1000, 100}});
        state->discInterface.emplace();
        state->discInterface->dmaAddress = 0x400000;
        diagnostics.ObserveDisc(*state, {0xa8000000u, 0x400, 100}, 100, true);
        state->gpr[4] = 0x80400000u;
        diagnostics.ObserveDecoder(*state);
        diagnostics.ObserveDisc(*state, {0xa8000000u, 0x400, 100}, 100, false, "I/O\n\"failure\"\\");
        OriginalStartupResult result{};
        result.reason = "No native translation registered at test.\n\t";
        result.pc = state->pc = 0xcc005000u; // DSP mailbox reads must not be issued by capture.
        state->dspHardware->fromDspMailbox = 0x80001234u;
        state->gpr[1] = 0xfffffffcu; // Invalid/overflowing backchain must stop safely.
        const auto memory = state->memory;
        const auto json = diagnostics.Json(*state, result, 100, 128);
        Require(json.find("\"recent_blocks\": [\"0x00000108\",\"0x0000010C\",\"0x00000110\"]") != std::string::npos,
                "History ring lost chronological ordering.");
        Require(json.find("\"instruction\": null") != std::string::npos &&
                    json.find("\"instruction_trace_enabled\": false") != std::string::npos &&
                    json.find("\"recent_instructions\": []") != std::string::npos &&
                    json.find("\"stack_backchain\": []") != std::string::npos &&
                    json.find("test\\\"file.clz") != std::string::npos &&
                    json.find("I/O\\u000a\\\"failure\\\"\\\\") != std::string::npos &&
                    state->dspHardware->fromDspMailbox == 0x80001234u && state->memory == memory,
                "Capture touched MMIO/RAM, failed invalid-stack checks, or emitted unescaped strings.");
        Require(ClassifyStartupStop(result) == "missing_translation", "Missing binding not classified.");
        result.stop = OriginalStartupStop::Budget;
        Require(ClassifyStartupStop(result) == "execution_limit", "Execution limit misclassified.");
        StartupDiagnostics disabled(0);
        disabled.ObserveBlock(0x100);
        Require(disabled.Json(*state, result, 1, 1).find("\"recent_blocks\": []") != std::string::npos,
                "Disabled history retained blocks.");
        bool rejected = false;
        try { StartupDiagnostics excessive(65537); } catch (const std::exception&) { rejected = true; }
        Require(rejected, "Unbounded trace capacity accepted.");
        rejected = false;
        try { StartupDiagnostics excessiveInstructionTrace(0, 65537); }
        catch (const std::exception&) { rejected = true; }
        Require(rejected, "Unbounded instruction trace capacity accepted.");
        StartupDiagnostics instructionTrace(0, 2);
        instructionTrace.ObserveInstruction(0x1000, 0x60000000);
        instructionTrace.ObserveInstruction(0x1004, 0x38600000);
        instructionTrace.ObserveInstruction(0x1008, 0x4e800020);
        const auto instructionJson = instructionTrace.Json(*state, result, 1, 1);
        Require(instructionJson.find("\"observed_instruction_entries\": 3") != std::string::npos &&
                    instructionJson.find("\"recent_instructions\": [{\"pc\":\"0x00001004\",\"word\":\"0x38600000\"},{\"pc\":\"0x00001008\",\"word\":\"0x4E800020\"}]") != std::string::npos &&
                    instructionJson.find("\"per-instruction history\"") == std::string::npos,
                "Instruction history did not retain bounded chronological PC/opcode records.");

        auto traced = std::make_unique<PpcState>(CreateOriginalStartupState(image));
        auto plain = std::make_unique<PpcState>(CreateOriginalStartupState(image));
        traced->readTimeBase = plain->readTimeBase = [] { return uint64_t(1000000); };
        traced->writeConsole = plain->writeConsole = [](const std::string&) {};
        StartupDiagnostics trace(128);
        const auto observed = RunOriginalStartup(dispatcher, *traced, 1000, 128,
            [&](uint32_t pc) { trace.ObserveBlock(pc); });
        const auto unobserved = RunOriginalStartup(dispatcher, *plain, 1000, 128);
        trace.Json(*traced, observed, 1000, 128);
        Require(observed.stop == unobserved.stop && observed.pc == unobserved.pc &&
                    observed.reason == unobserved.reason && traced->memory == plain->memory &&
                    std::equal(std::begin(traced->gpr), std::end(traced->gpr), std::begin(plain->gpr)) &&
                    traced->fpr == plain->fpr && traced->cr == plain->cr && traced->xer == plain->xer &&
                    traced->lr == plain->lr && traced->ctr == plain->ctr,
                "Diagnostic observation changed deterministic startup execution.");
        std::cout << "Historical function/thunk, bounded block/instruction traces, read-only capture, JSON escaping, classification, "
                     "resource correlation and deterministic startup parity passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
