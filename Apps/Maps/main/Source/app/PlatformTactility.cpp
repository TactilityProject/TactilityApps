// SPDX-License-Identifier: GPL-3.0-or-later
#include "../platform/Platform.h"

#include <tactility/log.h>
#include <tactility/memory.h>

#include <dirent.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <cstdarg>
#include <cstdio>

constexpr auto* TAG = "Maps";

namespace maps {

namespace {
/** What the app's own large blocks hold, so the memory log tells the app's share from the system's. */
std::atomic<uint32_t> largeBytes { 0 };
std::atomic<uint32_t> largeBlocks { 0 };
} // namespace

void* largeAlloc(size_t size) {
    const MemoryPolicy policy = {
        .required = 0,
        .desired = MEMORY_CAPABILITY_EXTERNAL,
        .alignment = 0,
    };
    void* pointer = memory_alloc_with_policy(size, &policy);
    if (pointer != nullptr) {
        largeBytes += static_cast<uint32_t>(heap_caps_get_allocated_size(pointer));
        largeBlocks++;
    }
    return pointer;
}

void* fastAlloc(size_t size) {
    const MemoryPolicy policy = {
        .required = 0,
        .desired = MEMORY_CAPABILITY_INTERNAL,
        .alignment = 0,
    };
    void* pointer = memory_alloc_with_policy(size, &policy);
    if (pointer != nullptr) {
        largeBytes += static_cast<uint32_t>(heap_caps_get_allocated_size(pointer));
        largeBlocks++;
    }
    return pointer;
}

void largeFree(void* pointer) {
    if (pointer != nullptr) {
        largeBytes -= static_cast<uint32_t>(heap_caps_get_allocated_size(pointer));
        largeBlocks--;
        memory_free(pointer);
    }
}

uint64_t nowUs() {
    return static_cast<uint64_t>(esp_timer_get_time());
}

uint32_t nowMs() {
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

namespace {

/**
 * The log mirror of debug scripts (setLogMirror): lines with the word, in PSRAM, under a mutex (made with the buffer;
 * a spinlock needs xPortEnterCriticalTimeout, which Tactility does not export).
 */
constexpr size_t MIRROR_BYTES = 8 * 1024;
/** Set once, after the buffer (release), read by every task that logs (acquire). */
std::atomic<SemaphoreHandle_t> mirrorLock { nullptr };
char* mirrorBuffer = nullptr;
size_t mirrorUsed = 0;
char mirrorWord[48] = "";

/** Whether the message contains one of the words ("Trails|Browse"). Called with the words copied under the lock. */
bool mirrored(const char* words, const char* message) {
    const char* word = words;
    while (*word != '\0') {
        const char* end = strchr(word, '|');
        const size_t length = end != nullptr ? static_cast<size_t>(end - word) : strlen(word);
        if (length > 0) {
            for (const char* p = message; *p != '\0'; p++) {
                if (strncmp(p, word, length) == 0) {
                    return true;
                }
            }
        }
        if (end == nullptr) {
            break;
        }
        word = end + 1;
    }
    return false;
}

void mirror(const char* level, const char* message) {
    SemaphoreHandle_t lock = mirrorLock.load(std::memory_order_acquire);
    if (lock == nullptr) {
        return;
    }
    char words[sizeof(mirrorWord)];
    xSemaphoreTake(lock, portMAX_DELAY);
    memcpy(words, mirrorWord, sizeof(words));
    xSemaphoreGive(lock);
    if (words[0] == '\0' || !mirrored(words, message)) {
        return;
    }
    char line[200];
    const int length = snprintf(line, sizeof(line), "%u %s%s\n", static_cast<unsigned>(esp_timer_get_time() / 1000), level, message);
    if (length <= 0) {
        return;
    }
    const size_t bytes = std::min(static_cast<size_t>(length), sizeof(line) - 1);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (mirrorUsed + bytes <= MIRROR_BYTES) {
        memcpy(mirrorBuffer + mirrorUsed, line, bytes);
        mirrorUsed += bytes;
    }
    xSemaphoreGive(lock);
}

} // namespace

void setLogMirror(const char* word) {
    // Called by the map task only (debug scripts)
    if (word != nullptr && word[0] != '\0' && mirrorLock.load(std::memory_order_acquire) == nullptr) {
        // Never freed: a few KB for the rest of the app session, only when a script asks for it. The lock is published
        // last: mirror() starts once it is there
        mirrorBuffer = static_cast<char*>(heap_caps_malloc(MIRROR_BYTES, MALLOC_CAP_SPIRAM));
        if (mirrorBuffer != nullptr) {
            SemaphoreHandle_t made = xSemaphoreCreateMutex();
            if (made == nullptr) {
                heap_caps_free(mirrorBuffer);
                mirrorBuffer = nullptr;
            }
            mirrorLock.store(made, std::memory_order_release);
        }
    }
    SemaphoreHandle_t lock = mirrorLock.load(std::memory_order_acquire);
    if (lock == nullptr) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    snprintf(mirrorWord, sizeof(mirrorWord), "%s", word != nullptr ? word : "");
    xSemaphoreGive(lock);
}

size_t peekMirroredLog(char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return 0;
    }
    out[0] = '\0';
    SemaphoreHandle_t lock = mirrorLock.load(std::memory_order_acquire);
    if (lock == nullptr) {
        return 0;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    size_t taken = 0;
    if (mirrorBuffer != nullptr && mirrorUsed > 0) {
        // Whole lines that fit
        taken = std::min(mirrorUsed, size - 1);
        while (taken > 0 && mirrorBuffer[taken - 1] != '\n') {
            taken--;
        }
        memcpy(out, mirrorBuffer, taken);
    }
    xSemaphoreGive(lock);
    out[taken] = '\0';
    return taken;
}

void dropMirroredLog(size_t bytes) {
    SemaphoreHandle_t lock = mirrorLock.load(std::memory_order_acquire);
    if (lock == nullptr || bytes == 0) {
        return;
    }
    xSemaphoreTake(lock, portMAX_DELAY);
    // Only the map task takes lines out, so the first bytes are still the ones it wrote
    const size_t dropped = std::min(bytes, mirrorUsed);
    memmove(mirrorBuffer, mirrorBuffer + dropped, mirrorUsed - dropped);
    mirrorUsed -= dropped;
    xSemaphoreGive(lock);
}

void logInfo(const char* format, ...) {
    char message[160];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    LOG_I(TAG, "%s", message);
    mirror("", message);
}

void logError(const char* format, ...) {
    char message[160];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    LOG_E(TAG, "%s", message);
    mirror("E ", message);
}

void logMemory(const char* label) {
    LOG_I(TAG, "Memory %s: internal %u free (largest %u), DMA %u free (largest %u), PSRAM %u free (largest %u), app large blocks %u in %u",
        label,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)),
        static_cast<unsigned>(largeBytes.load()),
        static_cast<unsigned>(largeBlocks.load()));
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
