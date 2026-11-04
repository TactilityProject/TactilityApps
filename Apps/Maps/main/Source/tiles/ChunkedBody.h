// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * A downloaded file in pieces of CHUNK_BYTES, so a tile of a megabyte needs no single free block of that size: late
 * in a session PSRAM has the room, but in smaller blocks.
 */
class ChunkedBody {
public:
    static constexpr size_t CHUNK_BYTES = 64 * 1024;
    static constexpr int MAX_CHUNKS = 64;
    /** After a file, pieces beyond this many go back to the heap. */
    static constexpr int KEEP_CHUNKS = 2;

    ChunkedBody() = default;
    ~ChunkedBody();
    ChunkedBody(const ChunkedBody&) = delete;
    ChunkedBody& operator=(const ChunkedBody&) = delete;

    void clear() {
        length = 0;
        error = false;
    }
    /** Clears and frees every piece. */
    void release();
    /** Clears and frees the pieces beyond KEEP_CHUNKS. */
    void trim();
    /** Room for total bytes, without marking a failure when there is none. */
    bool ensure(size_t total);
    void putBytes(const void* bytes, size_t count);
    void put(uint8_t value) { putBytes(&value, 1); }
    /** Marks the body as failed, for a file that is too large. */
    void fail() { error = true; }

    size_t size() const { return length; }
    bool failed() const { return error; }
    /** The pieces holding the bytes, in order; every one but the last is full. */
    int chunkCount() const { return static_cast<int>((length + CHUNK_BYTES - 1) / CHUNK_BYTES); }
    const uint8_t* chunk(int index) const { return chunks[index]; }
    size_t chunkSize(int index) const;
    /** The bytes in one piece, or nullptr when they span several. */
    const uint8_t* contiguous() const { return length <= CHUNK_BYTES ? chunks[0] : nullptr; }

private:
    uint8_t* chunks[MAX_CHUNKS] = {};
    int allocated = 0;
    size_t length = 0;
    bool error = false;
};

} // namespace maps
