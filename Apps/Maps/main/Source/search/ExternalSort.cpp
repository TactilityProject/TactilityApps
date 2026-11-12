// SPDX-License-Identifier: GPL-3.0-or-later
#include "ExternalSort.h"

#include "../platform/Platform.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

namespace maps {

namespace {

constexpr uint32_t OUT_BUFFER = 4096;
constexpr uint32_t MIN_BUFFER = 16 * 1024;

uint32_t get16(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8;
}

void put16(uint8_t* p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

int compareDefault(const uint8_t* a, uint32_t aSize, const uint8_t* b, uint32_t bSize, void*) {
    const uint32_t common = aSize < bSize ? aSize : bSize;
    const int c = common > 0 ? memcmp(a, b, common) : 0;
    if (c != 0) {
        return c;
    }
    return aSize < bSize ? -1 : (aSize > bSize ? 1 : 0);
}

} // namespace

/** A run in a run file: where it starts and how many bytes it has. */
struct RunInfo {
    uint32_t offset;
    uint32_t size;
};

/** One run being read in a merge: a window of the file in its own buffer, at least one whole record in it. */
struct RunReader {
    uint8_t* buffer;
    uint32_t filled;
    uint32_t at;
    /** File position of the next byte not yet in the buffer, and the run's end. */
    uint32_t next;
    uint32_t end;
    bool done;
};

struct ExternalSorter::Impl {
    ScratchFiles* files = nullptr;
    char runPath[136] = {};
    char mergePath[136] = {};
    Compare compare = nullptr;
    void* context = nullptr;
    uint32_t bufferBytes = 0;
    // Filling
    uint8_t* buffer = nullptr;
    uint32_t used = 0;
    uint32_t count = 0;
    // Runs in the run file, and the merge
    RunInfo* runs = nullptr;
    uint32_t runCapacity = 0;
    uint32_t runTotal = 0;
    uint32_t runFileSize = 0;
    bool runFileExists = false;
    bool mergeFileExists = false;
    RunReader readers[MAX_FAN_IN] = {};
    uint8_t* readMemory = nullptr;
    uint32_t readerCount = 0;
    int readHandle = -1;
    int lastReader = -1;
    // Memory mode: served from the sorted buffer
    bool memoryMode = false;
    uint32_t memoryAt = 0;
    bool finished = false;
    uint8_t out[OUT_BUFFER] = {};
    uint32_t outUsed = 0;
    size_t held = 0;
};

ExternalSorter::~ExternalSorter() {
    end();
}

namespace {

void setError(char* error, size_t size, const char* format, ...) {
    if (error[0] != '\0') {
        return;
    }
    va_list args;
    va_start(args, format);
    vsnprintf(error, size, format, args);
    va_end(args);
}

} // namespace

bool ExternalSorter::begin(ScratchFiles* files, const char* prefix, uint32_t bufferBytes, Compare compare, void* context) {
    end();
    error[0] = '\0';
    records = 0;
    runs = 0;
    written = 0;
    peak = 0;
    if (files == nullptr || prefix == nullptr || bufferBytes < MIN_BUFFER) {
        setError(error, sizeof(error), "bad sort setup");
        return false;
    }
    void* memory = largeAlloc(sizeof(Impl));
    if (memory == nullptr) {
        setError(error, sizeof(error), "no memory for a sort");
        return false;
    }
    impl = new (memory) Impl();
    impl->files = files;
    snprintf(impl->runPath, sizeof(impl->runPath), "%s.run", prefix);
    snprintf(impl->mergePath, sizeof(impl->mergePath), "%s.mrg", prefix);
    impl->compare = compare != nullptr ? compare : compareDefault;
    impl->context = context;
    impl->bufferBytes = bufferBytes;
    impl->buffer = static_cast<uint8_t*>(largeAlloc(bufferBytes));
    if (impl->buffer == nullptr) {
        setError(error, sizeof(error), "no memory for the sort buffer (%u bytes)", static_cast<unsigned>(bufferBytes));
        end();
        return false;
    }
    impl->held = sizeof(Impl) + bufferBytes;
    peak = impl->held;
    return true;
}

namespace {

/** The offsets of the records sit at the end of the buffer, the merge sort's second array before them. */
uint32_t* offsetsOf(uint8_t* buffer, uint32_t bufferBytes, uint32_t count) {
    return reinterpret_cast<uint32_t*>(buffer + bufferBytes) - count;
}

/** A stable bottom-up merge sort of record offsets. */
void sortOffsets(const uint8_t* records, uint32_t* a, uint32_t* b, uint32_t count, ExternalSorter::Compare compare, void* context) {
    uint32_t* from = a;
    uint32_t* to = b;
    for (uint32_t width = 1; width < count; width *= 2) {
        for (uint32_t left = 0; left < count; left += 2 * width) {
            const uint32_t middle = left + width < count ? left + width : count;
            const uint32_t right = left + 2 * width < count ? left + 2 * width : count;
            uint32_t i = left;
            uint32_t j = middle;
            uint32_t k = left;
            while (i < middle && j < right) {
                const uint8_t* x = records + from[i];
                const uint8_t* y = records + from[j];
                // Take the right one only when strictly smaller: equal records keep their order
                if (compare(y + 2, get16(y), x + 2, get16(x), context) < 0) {
                    to[k++] = from[j++];
                } else {
                    to[k++] = from[i++];
                }
            }
            while (i < middle) {
                to[k++] = from[i++];
            }
            while (j < right) {
                to[k++] = from[j++];
            }
        }
        uint32_t* swap = from;
        from = to;
        to = swap;
    }
    if (from != a) {
        memcpy(a, from, count * sizeof(uint32_t));
    }
}

} // namespace

/**
 * Sorts the buffer's records (their offsets). The offsets were stored from the end of the buffer downwards, newest
 * first: they are put in add() order first, so the stable sort keeps equal records in that order. The merge sort's
 * second array lies just before the offsets.
 */
static void sortBuffer(uint8_t* buffer, uint32_t bufferBytes, uint32_t count, ExternalSorter::Compare compare, void* context) {
    uint32_t* offsets = offsetsOf(buffer, bufferBytes, count);
    for (uint32_t i = 0, j = count; i + 1 < j; i++, j--) {
        const uint32_t swap = offsets[i];
        offsets[i] = offsets[j - 1];
        offsets[j - 1] = swap;
    }
    uint32_t* temp = offsets - count;
    sortOffsets(buffer, offsets, temp, count, compare, context);
}

bool ExternalSorter::writeRun() {
    sortBuffer(impl->buffer, impl->bufferBytes, impl->count, impl->compare, impl->context);
    const uint32_t* offsets = offsetsOf(impl->buffer, impl->bufferBytes, impl->count);
    if (impl->runTotal == impl->runCapacity) {
        const uint32_t capacity = impl->runCapacity < 16 ? 16 : impl->runCapacity * 2;
        auto* grown = static_cast<RunInfo*>(largeAlloc(capacity * sizeof(RunInfo)));
        if (grown == nullptr) {
            setError(error, sizeof(error), "no memory for the run list");
            return false;
        }
        if (impl->runs != nullptr) {
            memcpy(grown, impl->runs, impl->runTotal * sizeof(RunInfo));
            largeFree(impl->runs);
        }
        impl->held += (capacity - impl->runCapacity) * sizeof(RunInfo);
        peak = impl->held > peak ? impl->held : peak;
        impl->runs = grown;
        impl->runCapacity = capacity;
    }
    const int handle = impl->runFileExists ? impl->files->openAppend(impl->runPath) : impl->files->create(impl->runPath);
    if (handle < 0) {
        setError(error, sizeof(error), "cannot open the run file");
        return false;
    }
    impl->runFileExists = true;
    bool ok = true;
    uint32_t runBytes = 0;
    impl->outUsed = 0;
    for (uint32_t i = 0; i < impl->count && ok; i++) {
        const uint8_t* r = impl->buffer + offsets[i];
        const uint32_t total = 2 + get16(r);
        if (impl->outUsed + total > OUT_BUFFER) {
            ok = impl->files->write(handle, impl->out, impl->outUsed);
            impl->outUsed = 0;
        }
        memcpy(impl->out + impl->outUsed, r, total);
        impl->outUsed += total;
        runBytes += total;
    }
    if (ok && impl->outUsed > 0) {
        ok = impl->files->write(handle, impl->out, impl->outUsed);
    }
    ok = impl->files->close(handle) && ok;
    if (!ok) {
        setError(error, sizeof(error), "cannot write run %u", static_cast<unsigned>(impl->runTotal));
        return false;
    }
    impl->runs[impl->runTotal++] = { impl->runFileSize, runBytes };
    impl->runFileSize += runBytes;
    written += runBytes;
    runs++;
    impl->used = 0;
    impl->count = 0;
    return true;
}

bool ExternalSorter::add(const void* record, uint32_t size) {
    if (impl == nullptr || impl->finished || failed()) {
        return false;
    }
    if (size == 0 || size > MAX_RECORD) {
        setError(error, sizeof(error), "a sort record of %u bytes", static_cast<unsigned>(size));
        return false;
    }
    // Per record: its length, its bytes, its offset and the merge sort's second offset
    if (impl->used + 2 + size + (impl->count + 1) * 8 > impl->bufferBytes) {
        if (impl->count == 0) {
            setError(error, sizeof(error), "the sort buffer is too small");
            return false;
        }
        if (!writeRun()) {
            return false;
        }
    }
    uint8_t* at = impl->buffer + impl->used;
    put16(at, size);
    memcpy(at + 2, record, size);
    impl->count++;
    offsetsOf(impl->buffer, impl->bufferBytes, impl->count)[0] = impl->used;
    impl->used += 2 + size;
    records++;
    return true;
}

namespace {

/** Reads more of a run into its buffer, keeping the unread bytes. False on a read error. */
bool refill(ScratchFiles* files, int handle, RunReader& r) {
    if (r.at > 0) {
        memmove(r.buffer, r.buffer + r.at, r.filled - r.at);
        r.filled -= r.at;
        r.at = 0;
    }
    uint32_t want = ExternalSorter::READ_BUFFER - r.filled;
    if (want > r.end - r.next) {
        want = r.end - r.next;
    }
    if (want == 0) {
        return true;
    }
    if (!files->seek(handle, r.next)) {
        return false;
    }
    uint32_t got = 0;
    while (got < want) {
        const int n = files->read(handle, r.buffer + r.filled + got, want - got);
        if (n <= 0) {
            return false;
        }
        got += static_cast<uint32_t>(n);
    }
    r.filled += got;
    r.next += got;
    return true;
}

/** Makes sure the reader holds a whole record at r.at, or marks it done. False on a read error or a broken run. */
bool ready(ScratchFiles* files, int handle, RunReader& r) {
    if (r.done) {
        return true;
    }
    if (r.filled - r.at < 2 || r.filled - r.at < 2 + get16(r.buffer + r.at)) {
        if (!refill(files, handle, r)) {
            return false;
        }
    }
    if (r.filled - r.at == 0) {
        r.done = true;
        return true;
    }
    return r.filled - r.at >= 2 && r.filled - r.at >= 2 + get16(r.buffer + r.at);
}

/** The reader with the smallest record (the lowest run on a tie, which keeps the sort stable); -1 when all are done. */
int smallest(RunReader* readers, uint32_t count, ExternalSorter::Compare compare, void* context) {
    int best = -1;
    for (uint32_t i = 0; i < count; i++) {
        const RunReader& r = readers[i];
        if (r.done) {
            continue;
        }
        if (best < 0) {
            best = static_cast<int>(i);
            continue;
        }
        const RunReader& b = readers[best];
        const uint8_t* x = r.buffer + r.at;
        const uint8_t* y = b.buffer + b.at;
        if (compare(x + 2, get16(x), y + 2, get16(y), context) < 0) {
            best = static_cast<int>(i);
        }
    }
    return best;
}

} // namespace

bool ExternalSorter::finish() {
    if (impl == nullptr || impl->finished || failed()) {
        return false;
    }
    impl->finished = true;
    if (impl->runTotal == 0) {
        sortBuffer(impl->buffer, impl->bufferBytes, impl->count, impl->compare, impl->context);
        impl->memoryMode = true;
        impl->memoryAt = 0;
        return true;
    }
    if (impl->count > 0 && !writeRun()) {
        return false;
    }
    // The record buffer is not needed any more: the merge has its own buffers
    largeFree(impl->buffer);
    impl->buffer = nullptr;
    impl->held -= impl->bufferBytes;
    impl->readMemory = static_cast<uint8_t*>(largeAlloc(MAX_FAN_IN * READ_BUFFER));
    if (impl->readMemory == nullptr) {
        setError(error, sizeof(error), "no memory for the merge buffers");
        return false;
    }
    impl->held += MAX_FAN_IN * READ_BUFFER;
    peak = impl->held > peak ? impl->held : peak;
    for (uint32_t i = 0; i < MAX_FAN_IN; i++) {
        impl->readers[i].buffer = impl->readMemory + i * READ_BUFFER;
    }
    // Merge passes until at most MAX_FAN_IN runs are left: groups of consecutive runs into the other file
    while (impl->runTotal > MAX_FAN_IN) {
        const int input = impl->files->openRead(impl->runPath);
        const int output = input >= 0 ? impl->files->create(impl->mergePath) : -1;
        if (input < 0 || output < 0) {
            if (input >= 0) {
                impl->files->close(input);
            }
            setError(error, sizeof(error), "cannot open the files of a merge pass");
            return false;
        }
        impl->mergeFileExists = true;
        uint32_t newTotal = 0;
        uint32_t outputSize = 0;
        bool ok = true;
        for (uint32_t first = 0; first < impl->runTotal && ok; first += MAX_FAN_IN) {
            const uint32_t group = impl->runTotal - first < MAX_FAN_IN ? impl->runTotal - first : MAX_FAN_IN;
            for (uint32_t i = 0; i < group; i++) {
                RunReader& r = impl->readers[i];
                r.filled = 0;
                r.at = 0;
                r.next = impl->runs[first + i].offset;
                r.end = impl->runs[first + i].offset + impl->runs[first + i].size;
                r.done = false;
                ok = ok && ready(impl->files, input, r);
            }
            const uint32_t start = outputSize;
            impl->outUsed = 0;
            while (ok) {
                const int best = smallest(impl->readers, group, impl->compare, impl->context);
                if (best < 0) {
                    break;
                }
                RunReader& r = impl->readers[best];
                const uint32_t total = 2 + get16(r.buffer + r.at);
                if (impl->outUsed + total > OUT_BUFFER) {
                    ok = impl->files->write(output, impl->out, impl->outUsed);
                    impl->outUsed = 0;
                }
                memcpy(impl->out + impl->outUsed, r.buffer + r.at, total);
                impl->outUsed += total;
                outputSize += total;
                r.at += total;
                ok = ok && ready(impl->files, input, r);
            }
            if (ok && impl->outUsed > 0) {
                ok = impl->files->write(output, impl->out, impl->outUsed);
                impl->outUsed = 0;
            }
            // Earlier entries are already read: the merged runs can take their places
            impl->runs[newTotal++] = { start, outputSize - start };
            runs++;
        }
        impl->files->close(input);
        ok = impl->files->close(output) && ok;
        if (!ok) {
            setError(error, sizeof(error), "a merge pass failed");
            return false;
        }
        written += outputSize;
        impl->runTotal = newTotal;
        impl->runFileSize = outputSize;
        // The merged file is the run file now
        if (!impl->files->rename(impl->mergePath, impl->runPath)) {
            setError(error, sizeof(error), "cannot rename the merged runs");
            return false;
        }
        impl->mergeFileExists = false;
    }
    // The final merge is done by next()
    impl->readHandle = impl->files->openRead(impl->runPath);
    if (impl->readHandle < 0) {
        setError(error, sizeof(error), "cannot open the run file");
        return false;
    }
    impl->readerCount = impl->runTotal;
    for (uint32_t i = 0; i < impl->readerCount; i++) {
        RunReader& r = impl->readers[i];
        r.filled = 0;
        r.at = 0;
        r.next = impl->runs[i].offset;
        r.end = impl->runs[i].offset + impl->runs[i].size;
        r.done = false;
        if (!ready(impl->files, impl->readHandle, r)) {
            setError(error, sizeof(error), "cannot read run %u", static_cast<unsigned>(i));
            return false;
        }
    }
    impl->lastReader = -1;
    return true;
}

bool ExternalSorter::next(const uint8_t*& data, uint32_t& size) {
    if (impl == nullptr || !impl->finished || failed()) {
        return false;
    }
    if (impl->memoryMode) {
        if (impl->memoryAt >= impl->count) {
            return false;
        }
        const uint32_t* offsets = offsetsOf(impl->buffer, impl->bufferBytes, impl->count);
        const uint8_t* r = impl->buffer + offsets[impl->memoryAt++];
        size = get16(r);
        data = r + 2;
        return true;
    }
    // Step past the record returned last time (its bytes stayed valid until now)
    if (impl->lastReader >= 0) {
        RunReader& r = impl->readers[impl->lastReader];
        r.at += 2 + get16(r.buffer + r.at);
        if (!ready(impl->files, impl->readHandle, r)) {
            setError(error, sizeof(error), "cannot read run %d", impl->lastReader);
            return false;
        }
        impl->lastReader = -1;
    }
    const int best = smallest(impl->readers, impl->readerCount, impl->compare, impl->context);
    if (best < 0) {
        return false;
    }
    const RunReader& r = impl->readers[best];
    size = get16(r.buffer + r.at);
    data = r.buffer + r.at + 2;
    impl->lastReader = best;
    return true;
}

void ExternalSorter::end() {
    if (impl == nullptr) {
        return;
    }
    if (impl->readHandle >= 0) {
        impl->files->close(impl->readHandle);
        impl->readHandle = -1;
    }
    if (impl->runFileExists) {
        impl->files->remove(impl->runPath);
    }
    if (impl->mergeFileExists) {
        impl->files->remove(impl->mergePath);
    }
    largeFree(impl->readMemory);
    largeFree(impl->buffer);
    largeFree(impl->runs);
    impl->~Impl();
    largeFree(impl);
    impl = nullptr;
}

} // namespace maps
