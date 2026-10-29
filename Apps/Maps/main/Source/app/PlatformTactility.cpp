// SPDX-License-Identifier: GPL-3.0-or-later
#include "../platform/Platform.h"

#include <tactility/log.h>
#include <tactility/memory.h>

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>

constexpr auto* TAG = "Maps";

namespace maps {

void* largeAlloc(size_t size) {
    const MemoryPolicy policy = {
        .required = 0,
        .desired = MEMORY_CAPABILITY_EXTERNAL,
        .alignment = 0,
    };
    return memory_alloc_with_policy(size, &policy);
}

void* fastAlloc(size_t size) {
    const MemoryPolicy policy = {
        .required = 0,
        .desired = MEMORY_CAPABILITY_INTERNAL,
        .alignment = 0,
    };
    return memory_alloc_with_policy(size, &policy);
}

void largeFree(void* pointer) {
    if (pointer != nullptr) {
        memory_free(pointer);
    }
}

uint64_t nowUs() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

void logInfo(const char* format, ...) {
    char message[160];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    LOG_I(TAG, "%s", message);
}

void logError(const char* format, ...) {
    char message[160];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    LOG_E(TAG, "%s", message);
}

void logMemory(const char* label) {
    LOG_I(TAG, "Memory %s: internal %u free (largest %u), PSRAM %u free",
        label,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
}

bool fileExists(const char* path) {
    struct stat info {};
    return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

bool fileSize(const char* path, uint32_t& size) {
    struct stat info {};
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) {
        return false;
    }
    size = static_cast<uint32_t>(info.st_size);
    return true;
}

bool removeFile(const char* path) {
    return remove(path) == 0;
}

bool removeFolder(const char* path) {
    return rmdir(path) == 0;
}

bool listDirectory(const char* path, DirectoryCallback callback, void* context) {
    DIR* dir = opendir(path);
    if (dir == nullptr) {
        return false;
    }
    while (const dirent* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!callback(entry->d_name, entry->d_type == DT_DIR, context)) {
            break;
        }
    }
    closedir(dir);
    return true;
}

} // namespace maps
