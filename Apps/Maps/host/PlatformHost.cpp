// SPDX-License-Identifier: GPL-3.0-or-later
#include "../main/Source/platform/Platform.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace maps {

void* largeAlloc(size_t size) {
    return malloc(size);
}

void* fastAlloc(size_t size) {
    return malloc(size);
}

void largeFree(void* pointer) {
    free(pointer);
}

uint64_t nowUs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count());
}

uint32_t nowMs() {
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void logInfo(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    printf("\n");
}

void logError(const char* format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, "ERROR: ");
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void logMemory(const char* label) {
    (void)label;
}

bool fileExists(const char* path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

bool fileSize(const char* path, uint32_t& size) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return false;
    }
    size = static_cast<uint32_t>(std::filesystem::file_size(path, error));
    return !error;
}

bool removeFile(const char* path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && std::filesystem::remove(path, error);
}

bool removeFolder(const char* path) {
    std::error_code error;
    return std::filesystem::is_directory(path, error) && std::filesystem::is_empty(path, error) && std::filesystem::remove(path, error);
}

bool listDirectory(const char* path, DirectoryCallback callback, void* context) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(path, error)) {
        const std::string name = entry.path().filename().string();
        if (!callback(name.c_str(), entry.is_directory(), context)) {
            break;
        }
    }
    return !error;
}

} // namespace maps
