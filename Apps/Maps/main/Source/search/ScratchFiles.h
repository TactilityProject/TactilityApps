// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>

namespace maps {

/**
 * Files for the streaming index build (M6): sorted runs, per-tile extracts and the index being written. The host
 * uses stdio (StdioScratchFiles); the device does every call on its I/O task and keeps at most two files really open
 * (the card's FAT mount allows four for the whole system), reopening the others at their position when used. Callers
 * write and read in pieces of a few KB, never byte by byte. Paths are full paths. A handle is a small number >= 0.
 */
class ScratchFiles {
public:
    static constexpr int MAX_OPEN = 24;

    virtual ~ScratchFiles() = default;
    /** Creates (or empties) a file for writing; -1 on error. */
    virtual int create(const char* path) = 0;
    /** Opens a file for reading; -1 on error. */
    virtual int openRead(const char* path) = 0;
    /** Opens a file for writing at its end (creates it when missing); -1 on error. */
    virtual int openAppend(const char* path) = 0;
    virtual bool write(int handle, const void* data, uint32_t size) = 0;
    /** Reads up to size bytes at the current position: the count, 0 at the end, -1 on error. */
    virtual int read(int handle, void* data, uint32_t size) = 0;
    /** Moves the position of a file opened for reading. */
    virtual bool seek(int handle, uint32_t offset) = 0;
    /** Closes the handle; for a written file false when its data could not be written. */
    virtual bool close(int handle) = 0;
    /** Removes a file; true when it is gone afterwards (also when it was not there). */
    virtual bool remove(const char* path) = 0;
    /** Renames a file, replacing the target. */
    virtual bool rename(const char* from, const char* to) = 0;
};

/** ScratchFiles over stdio, for the host tools and tests. */
class StdioScratchFiles final : public ScratchFiles {
public:
    ~StdioScratchFiles() override {
        for (FILE*& f : files) {
            if (f != nullptr) {
                fclose(f);
                f = nullptr;
            }
        }
    }
    int create(const char* path) override { return put(fopen(path, "wb")); }
    int openRead(const char* path) override { return put(fopen(path, "rb")); }
    int openAppend(const char* path) override { return put(fopen(path, "ab")); }
    bool write(int handle, const void* data, uint32_t size) override {
        FILE* f = at(handle);
        return f != nullptr && (size == 0 || fwrite(data, 1, size, f) == size);
    }
    int read(int handle, void* data, uint32_t size) override {
        FILE* f = at(handle);
        if (f == nullptr) {
            return -1;
        }
        const size_t n = fread(data, 1, size, f);
        return n == 0 && ferror(f) ? -1 : static_cast<int>(n);
    }
    bool seek(int handle, uint32_t offset) override {
        FILE* f = at(handle);
        return f != nullptr && fseek(f, static_cast<long>(offset), SEEK_SET) == 0;
    }
    bool close(int handle) override {
        FILE* f = at(handle);
        if (f == nullptr) {
            return false;
        }
        files[handle] = nullptr;
        return fclose(f) == 0;
    }
    bool remove(const char* path) override {
        ::remove(path);
        FILE* f = fopen(path, "rb");
        if (f != nullptr) {
            fclose(f);
            return false;
        }
        return true;
    }
    bool rename(const char* from, const char* to) override {
        ::remove(to);
        return ::rename(from, to) == 0;
    }
    /** How many files are open now (for tests that check every handle is closed). */
    int openCount() const {
        int n = 0;
        for (FILE* f : files) {
            n += f != nullptr ? 1 : 0;
        }
        return n;
    }

private:
    int put(FILE* f) {
        if (f == nullptr) {
            return -1;
        }
        for (int i = 0; i < MAX_OPEN; i++) {
            if (files[i] == nullptr) {
                files[i] = f;
                return i;
            }
        }
        fclose(f);
        return -1;
    }
    FILE* at(int handle) const { return handle >= 0 && handle < MAX_OPEN ? files[handle] : nullptr; }
    FILE* files[MAX_OPEN] = {};
};

} // namespace maps
