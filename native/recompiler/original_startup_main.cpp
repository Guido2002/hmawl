#include "original_startup.h"
#include "native_sram.h"

#include <charconv>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

size_t ParseLimit(const std::filesystem::path& text) {
    const auto value = text.string();
    size_t limit = 0;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), limit);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || limit == 0) {
        throw std::runtime_error("Execution limits must be positive decimal integers.");
    }
    return limit;
}

template <typename Character>
int RunCli(int argc, Character** argv) {
    if (argc < 3 || argc > 11 || (argc & 1) == 0 ||
        std::filesystem::path(argv[1]) != "--original-startup") {
        std::cerr << "Usage: hmawl_original_startup --original-startup <USA-main.dol>"
                     " [--max-blocks <count>] [--max-depth <count>] [--sram <64-byte-profile>]"
                     " [--rvz <USA-disc.rvz>]\n";
        return 2;
    }
    try {
        size_t blocks = 1000000;
        size_t depth = 128;
        bool sawBlocks = false, sawDepth = false, sawSram = false;
        std::filesystem::path rvzPath;
        auto sramPath = std::filesystem::path(argv[2]).parent_path() / "native-console.sram";
        for (int i = 3; i < argc; i += 2) {
            const auto option = std::filesystem::path(argv[i]).string();
            if (option == "--max-blocks" && !sawBlocks) {
                blocks = ParseLimit(argv[i + 1]);
                sawBlocks = true;
            } else if (option == "--max-depth" && !sawDepth) {
                depth = ParseLimit(argv[i + 1]);
                sawDepth = true;
            } else if (option == "--sram" && !sawSram) {
                sramPath = argv[i + 1];
                sawSram = true;
            } else if (option == "--rvz" && rvzPath.empty()) {
                rvzPath = argv[i + 1];
                if (rvzPath.empty()) throw std::runtime_error("Native RVZ disc path must not be empty.");
            } else {
                throw std::runtime_error("Unknown or repeated startup option: " + option);
            }
        }
        const auto image = LoadOriginalUsDol(argv[2]);
        auto state = CreateOriginalStartupState(image);
        std::unique_ptr<RvzImage> disc;
        if (!rvzPath.empty()) {
            disc = std::make_unique<RvzImage>(rvzPath);
            InstallOriginalStartupDisc(state, *disc);
        }
        BindNativeSram(state, LoadOrCreateNativeSram(sramPath));
        NativeDispatcher dispatcher;
        RegisterOriginalStartup(dispatcher, state);
        std::cout << "Executing hash-verified original USA DOL entry 0x80003154 "
                     "using compiled native blocks only.\n"
                     "Profile: loader IR/DR, EE disabled, coherent RAM, idle opt-in "
                     "PI/MI/DSP/AI/EXI/SI, native CP/PE and checked GX register-command FIFO "
                     "(non-AA RGB8 EFB/XFB copy/clear; draw/texture DMA unimplemented), native NTSC VI timing/configuration/interrupts "
                     "(pixel scanout unimplemented), cold PI reset, 24 MiB RAM BAT profile, disconnected digital cable, "
                     "empty memory-card slots/SP1 and disconnected controllers, native console "
                     "(bounded strings/32-bit integers), persisted SRAM reads (writes/RTC/ROM unimplemented); "
                     "16 MiB ARAM, explicit disconnected expansion, synchronous DMA, native AI sample clock/volume "
                     "(no PCM playback), explicit BIOS-free DSP task loader and compiled original task startup "
                     "(DSP ROM execution/mixing unimplemented); "
                     "162 MHz bus/486 MHz CPU metadata, steady-clock 40.5 MHz time base.\n";
        std::cout << "Native SRAM profile: " << sramPath.u8string() << '\n';
        if (disc) {
            std::cout << "Verified native disc boot metadata: " << rvzPath.u8string() << '\n'
                      << "Drive: native RVZ virtual drive, revision 1, no console firmware/date; "
                         "checked read/inquiry DMA, original DVD interrupts; expired timer exceptions unsupported.\n";
        }
        const auto result = RunOriginalStartup(dispatcher, state, blocks, depth);
        std::cerr << DescribeOriginalStartupStop(result) << '\n';
        if (state.dspHardware) {
            const auto& dsp = *state.dspHardware;
            std::cerr << "Native ARAM: " << dsp.aram.size() << " bytes, " << dsp.dmaTransfers
                      << " synchronous DMA transfers; disconnected expansion: "
                      << dsp.absentAramReads << " no-data reads, " << dsp.absentAramWrites
                      << " unreceived writes. DSP programs completed=" << dsp.completedPrograms << ".\n";
            if (dsp.task) {
                const auto& task = *dsp.task;
                std::cerr << "Native DSP task: " << task.loaderStarts << " BIOS-free loader starts, "
                          << task.instructionUploads << " IRAM uploads/" << task.uploadedBytes
                          << " bytes, CPU mail=" << task.cpuMailSent << "/" << task.cpuMailConsumed
                          << ", task IRQs=" << task.interrupts << ", DSP PC=0x" << std::hex
                          << dsp.instructionPc << std::dec << ", executed=" << dsp.executedInstructions
                          << ", DSP mail=" << dsp.sentMail << "/" << dsp.consumedMail << ".\n";
            }
        }
        if (state.audio) {
            const auto& ai = *state.audio;
            std::cerr << "Native AI: volume=0x" << std::hex << ai.volume << " control=0x"
                      << state.aiControl.value() << std::dec << ", " << ai.advancedSamples
                      << " timed sample-clock steps, " << ai.resets << " counter resets. No PCM playback.\n";
        }
        if (state.graphics) {
            const auto& gx = *state.graphics;
            std::cerr << "Native GX: " << gx.bursts << " actual 32-byte FIFO bursts, "
                      << gx.decodedCommands << " decoded register/NOP commands, "
                      << gx.commands.size() << " pending command bytes, "
                      << gx.gathered.size() << " gathered bytes. No rendered frame.\n";
            if (gx.efb) {
                const auto& efb = *gx.efb;
                std::cerr << "Native EFB: " << efb.copies << " XFB RAM copies, " << efb.clears
                          << " copy-clear operations; " << efb.undefinedCopies
                          << " copies contain undefined power-on pixels (not rendered frames).\n";
                if (efb.lastCopy) {
                    const auto& copy = *efb.lastCopy;
                    std::cerr << "Last XFB copy: " << copy.width << "x" << copy.height
                              << " stride=" << copy.stride << " address=0x" << std::hex << copy.address
                              << std::dec << " undefined pixels=" << copy.undefinedPixels << ".\n";
                }
            }
        }
        if (state.video) {
            const auto& vi = *state.video;
            std::cerr << "Native VI: " << vi.writes << " register writes, " << vi.resets
                      << " reset(s), " << vi.fields << " timed fields, "
                      << (state.externalInterrupt ? state.externalInterrupt->returned : 0)
                      << " total returned external interrupts. SRR0=0x" << std::hex << state.srr0
                      << " SRR1=0x" << state.srr1 << std::dec << ", "
                      << state.threadContextReturns << " restored thread context(s). No pixel scanout.\n";
        }
        return result.stop == OriginalStartupStop::Budget ? 4 :
               result.stop == OriginalStartupStop::UnexpectedReturn ? 5 : 3;
    } catch (const std::exception& error) {
        std::cerr << "Original startup input/configuration error: " << error.what() << '\n';
        return 1;
    }
}

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return RunCli(argc, argv); }
#else
int main(int argc, char** argv) { return RunCli(argc, argv); }
#endif
