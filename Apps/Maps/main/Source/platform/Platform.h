// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/** Allocates a buffer, preferring external RAM where the device has it. */
void* largeAlloc(size_t size);
void largeFree(void* pointer);
/** Allocates a small buffer that is used very often, preferring internal RAM. Free it with largeFree(). */
void* fastAlloc(size_t size);

uint32_t nowMs();
uint64_t nowUs();

void logInfo(const char* format, ...);
void logError(const char* format, ...);

bool fileExists(const char* path);
/** The size of a regular file. Returns false when there is none. */
bool fileSize(const char* path, uint32_t& size);

bool removeFile(const char* path);
/** Removes an empty folder. */
bool removeFolder(const char* path);

/** Calls back for each entry of a folder until the callback returns false. */
using DirectoryCallback = bool (*)(const char* name, bool isDirectory, void* context);
bool listDirectory(const char* path, DirectoryCallback callback, void* context);

/** Logs free internal RAM and external RAM with a label. */
void logMemory(const char* label);

} // namespace maps
