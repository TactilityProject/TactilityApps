// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ScratchFiles.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Sorts a stream of variable-size records with a fixed amount of memory (M6 streaming index build): add() fills a
 * buffer; a full buffer is sorted and appended to the run file as the next run; finish() sorts the rest and merges the
 * runs; next() then returns the records in order. All runs of a sort are in one file (<prefix>.run, and <prefix>.mrg
 * for merge passes), so a merge reads through a single handle: the card's FAT mount allows only four open files for
 * the whole system. Records that compare equal keep the order of add() (the sort is stable). When all records fit the
 * buffer no file is written. Platform free: memory through largeAlloc (PSRAM on the device), files through
 * ScratchFiles.
 */
class ExternalSorter {
public:
    /** The largest record add() takes. */
    static constexpr uint32_t MAX_RECORD = 2048;
    /** At most this many runs are merged at once; more runs are first merged into longer runs. */
    static constexpr uint32_t MAX_FAN_IN = 16;
    /** The read buffer of one run while merging (holds at least one whole record). */
    static constexpr uint32_t READ_BUFFER = 4096;

    /** <0, 0, >0 like memcmp; context is the one given to begin(). */
    using Compare = int (*)(const uint8_t* a, uint32_t aSize, const uint8_t* b, uint32_t bSize, void* context);

    ExternalSorter() = default;
    ~ExternalSorter();
    ExternalSorter(const ExternalSorter&) = delete;
    ExternalSorter& operator=(const ExternalSorter&) = delete;

    /**
     * Starts a sort. The run files are <prefix>.run and <prefix>.mrg (prefix is a full path such as
     * "/sdcard/maps/x/srch/k"). bufferBytes (at least 16 KB) holds the records of one run and their index (2 + size
     * + 8 bytes per record); the merge needs MAX_FAN_IN * READ_BUFFER bytes instead (the buffer is freed first).
     * compare == nullptr compares the bytes (memcmp, then the shorter first).
     */
    bool begin(ScratchFiles* files, const char* prefix, uint32_t bufferBytes, Compare compare, void* context);
    /** Adds a record (1 to MAX_RECORD bytes). Only before finish(). */
    bool add(const void* record, uint32_t size);
    /** Sorts what is left and prepares the merge. Only once, after the last add(). */
    bool finish();
    /** The next record in order: data stays valid until the next call. False at the end or on an error (failed()). */
    bool next(const uint8_t*& data, uint32_t& size);
    /** Closes and removes the run files and frees the memory. Safe to call at any time, also twice. */
    void end();

    bool failed() const { return error[0] != '\0'; }
    const char* errorText() const { return error; }

    /** For logs and tests. */
    uint64_t recordCount() const { return records; }
    /** Runs made, merged runs included. */
    uint32_t runCount() const { return runs; }
    /** Bytes written to run files, intermediate merges included. */
    uint64_t bytesWritten() const { return written; }
    /** The most memory this sorter held at once. */
    size_t peakBytes() const { return peak; }

private:
    bool writeRun();
    struct Impl;
    Impl* impl = nullptr;
    uint64_t records = 0;
    uint32_t runs = 0;
    uint64_t written = 0;
    size_t peak = 0;
    char error[96] = {};
};

} // namespace maps
