#include "original_startup.h"
#include "native_sram.h"

#include <charconv>
#include <iostream>
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
    if (argc < 3 || argc > 9 || (argc & 1) == 0 ||
        std::filesystem::path(argv[1]) != "--original-startup") {
        std::cerr << "Usage: hmawl_original_startup --original-startup <USA-main.dol>"
                     " [--max-blocks <count>] [--max-depth <count>] [--sram <64-byte-profile>]\n";
        return 2;
    }
    try {
        size_t blocks = 1000000;
        size_t depth = 128;
        bool sawBlocks = false, sawDepth = false, sawSram = false;
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
            } else {
                throw std::runtime_error("Unknown or repeated startup option: " + option);
            }
        }
        const auto image = LoadOriginalUsDol(argv[2]);
        auto state = CreateOriginalStartupState(image);
        BindNativeSram(state, LoadOrCreateNativeSram(sramPath));
        NativeDispatcher dispatcher;
        RegisterOriginalStartup(dispatcher, state);
        std::cout << "Executing hash-verified original USA DOL entry 0x80003154 "
                     "using compiled native blocks only.\n"
                     "Profile: loader IR/DR, EE disabled, coherent RAM, idle opt-in "
                     "PI/MI/DSP/AI/EXI/SI, cold PI reset, 24 MiB RAM BAT profile, analog VI cable status, "
                     "empty memory-card slots/SP1 and disconnected controllers, native console "
                     "(bounded strings/32-bit integers), persisted SRAM reads (writes/RTC/ROM unimplemented); "
                     "16 MiB ARAM, compiled DSP init/native bootstrap (general DSP tasks/audio unimplemented); "
                     "162 MHz bus/486 MHz CPU metadata, steady-clock 40.5 MHz time base.\n";
        std::cout << "Native SRAM profile: " << sramPath.u8string() << '\n';
        const auto result = RunOriginalStartup(dispatcher, state, blocks, depth);
        std::cerr << DescribeOriginalStartupStop(result) << '\n';
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
