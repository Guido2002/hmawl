#ifndef HMAWL_NATIVE_CONSOLE_H
#define HMAWL_NATIVE_CONSOLE_H

#include "ppc_state.h"

#include <string>
#include <charconv>

inline std::string ReadGuestConsoleString(const PpcState& state, uint32_t address) {
    std::string text;
    for (unsigned length = 0; length < 4096; ++length) {
        if (address > UINT32_MAX - length) {
            throw std::runtime_error("Guest console string address overflow.");
        }
        const auto value = ppc_read_u8(state, address + length);
        if (value == 0) return text;
        text.push_back(static_cast<char>(value));
    }
    throw std::runtime_error("Guest console string exceeds the 4096-byte limit.");
}

inline void NativeGuestVprintf(PpcState& state) {
    if (!state.writeConsole) {
        throw std::runtime_error("Guest vprintf requires a native console sink.");
    }
    const auto format = ReadGuestConsoleString(state, state.gpr[3]);
    const uint32_t arguments = state.gpr[4];
    (void)ppc_memory_offset(state, arguments, 12);
    unsigned index = ppc_read_u8(state, arguments);
    if (index > 8) throw std::runtime_error("Invalid guest vprintf GPR argument index.");
    uint32_t overflow = ppc_read_u32(state, arguments + 4u);
    const uint32_t saved = ppc_read_u32(state, arguments + 8u);
    const auto nextArgument = [&]() {
        const bool inRegisters = index < 8;
        const uint64_t slot = inRegisters ? static_cast<uint64_t>(saved) + index++ * 4u :
            static_cast<uint64_t>(overflow);
        if (slot > UINT32_MAX - 3u) throw std::runtime_error("Guest vprintf argument address overflow.");
        const auto value = ppc_read_u32(state, static_cast<uint32_t>(slot));
        if (!inRegisters) {
            if (overflow > UINT32_MAX - 4u) throw std::runtime_error("Guest vprintf overflow area wraps.");
            overflow += 4u;
        }
        return value;
    };
    std::string text;
    for (size_t at = 0; at < format.size(); ++at) {
        if (format[at] != '%') {
            text.push_back(format[at]);
        } else {
            if (++at == format.size()) throw std::runtime_error("Incomplete guest vprintf format.");
            if (format[at] == '%') {
                text.push_back('%');
                continue;
            }
            const bool zero = format[at] == '0';
            unsigned width = 0;
            while (at < format.size() && format[at] >= '0' && format[at] <= '9') {
                const auto digit = static_cast<unsigned>(format[at++] - '0');
                if (width > (4096u - digit) / 10u) {
                    throw std::runtime_error("Guest vprintf width exceeds 4096 bytes.");
                }
                width = width * 10u + digit;
            }
            if (at == format.size()) throw std::runtime_error("Incomplete guest vprintf format.");
            const char conversion = format[at];
            if (conversion == 's' && width == 0 && !zero) {
                text += ReadGuestConsoleString(state, nextArgument());
            } else if (conversion == 'd' || conversion == 'i' || conversion == 'u' ||
                       conversion == 'x' || conversion == 'X') {
                uint32_t value = nextArgument();
                const bool negative = (conversion == 'd' || conversion == 'i') &&
                    (value & 0x80000000u) != 0;
                if (negative) value = 0u - value;
                char buffer[10];
                const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value,
                    conversion == 'x' || conversion == 'X' ? 16 : 10);
                if (result.ec != std::errc{}) throw std::runtime_error("Guest integer formatting failed.");
                std::string number(buffer, result.ptr);
                if (conversion == 'X') {
                    for (char& c : number) if (c >= 'a' && c <= 'f') c -= 'a' - 'A';
                }
                const auto length = number.size() + (negative ? 1u : 0u);
                const size_t padding = width > length ? width - length : 0;
                if (!zero) text.append(padding, ' ');
                if (negative) text.push_back('-');
                if (zero) text.append(padding, '0');
                text += number;
            } else {
                throw std::runtime_error("Unsupported guest vprintf conversion or flags.");
            }
        }
        if (text.size() > 65536) throw std::runtime_error("Guest vprintf output exceeds 64 KiB.");
    }
    if (text.size() > 65536) throw std::runtime_error("Guest vprintf output exceeds 64 KiB.");
    state.writeConsole(text);
    state.gpr[3] = static_cast<uint32_t>(text.size());
    state.pc = state.lr;
}

#endif
