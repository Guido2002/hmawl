#include "native_sram.h"

#include <fstream>
#include <system_error>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

NativeSramBytes LoadOrCreateNativeSram(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
#ifdef _WIN32
        const auto bytes = NewNativeSram();
        const auto handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                        FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (error != ERROR_FILE_EXISTS) {
                throw std::system_error(error, std::system_category(), "Could not create native SRAM profile");
            }
        } else {
            DWORD written = 0;
            DWORD failure = ERROR_SUCCESS;
            if (!WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
                failure = GetLastError();
            } else if (written != bytes.size()) {
                failure = ERROR_WRITE_FAULT;
            } else if (!FlushFileBuffers(handle)) {
                failure = GetLastError();
            }
            if (!CloseHandle(handle) && failure == ERROR_SUCCESS) failure = GetLastError();
            if (failure != ERROR_SUCCESS) {
                if (!DeleteFileW(path.c_str())) {
                    throw std::system_error(GetLastError(), std::system_category(),
                                            "Could not clean up failed native SRAM profile creation");
                }
                throw std::system_error(failure, std::system_category(), "Could not persist native SRAM profile");
            }
        }
#else
        throw std::runtime_error("Creating a native SRAM profile currently requires Windows.");
#endif
    }
    if (std::filesystem::file_size(path) != 64) {
        throw std::runtime_error("Native SRAM profile must contain exactly 64 bytes; refusing to reset it.");
    }
    NativeSramBytes bytes = {};
    std::ifstream input(path, std::ios::binary);
    input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
    if (!input || input.peek() != std::char_traits<char>::eof()) {
        throw std::runtime_error("Could not read the exact native SRAM profile.");
    }
    return bytes;
}
