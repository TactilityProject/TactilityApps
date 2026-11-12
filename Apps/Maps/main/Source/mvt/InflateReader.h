// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ByteInput.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Reads a gzip, zlib or uncompressed stream byte by byte, inflating with tinfl into a 32 KB ring window.
 * The format is detected from the first two bytes. Memory is allocated once in init().
 */
class InflateReader {
public:
    enum class Format : uint8_t {
        Raw,
        Gzip,
        Zlib
    };

    InflateReader() = default;
    ~InflateReader();
    InflateReader(const InflateReader&) = delete;
    InflateReader& operator=(const InflateReader&) = delete;

    bool init();
    /** Starts reading input from its beginning. */
    bool begin(ByteInput* input);

    /** Returns the next byte, or -1 at the end of the stream or on error. */
    inline int get() {
        if (pending == pendingEnd && !refill()) {
            return -1;
        }
        position++;
        return *pending++;
    }

    size_t read(uint8_t* buffer, size_t size);
    bool skip(size_t count);

    /** Bytes of output consumed since begin(). */
    size_t tell() const { return position; }
    bool failed() const { return error; }
    Format format() const { return streamFormat; }

private:
    bool refill();
    bool fillInput();
    bool skipGzipHeader();
    int getInputByte();

    ByteInput* input = nullptr;
    void* decompressor = nullptr;
    uint8_t* window = nullptr;
    uint8_t* inputBuffer = nullptr;
    size_t inputPosition = 0;
    size_t inputSize = 0;
    bool inputEnded = false;
    size_t windowOffset = 0;
    const uint8_t* pending = nullptr;
    const uint8_t* pendingEnd = nullptr;
    size_t position = 0;
    Format streamFormat = Format::Raw;
    bool finished = false;
    bool error = false;
};

} // namespace maps
