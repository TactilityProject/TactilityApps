// SPDX-License-Identifier: GPL-3.0-or-later
#include "InflateReader.h"

#include "../platform/Platform.h"

#include <cstring>
#include <initializer_list>
#include <miniz.h>

namespace maps {

namespace {

constexpr size_t WINDOW_SIZE = TINFL_LZ_DICT_SIZE;
constexpr size_t INPUT_BUFFER_SIZE = 4096;

tinfl_decompressor* asDecompressor(void* pointer) {
    return static_cast<tinfl_decompressor*>(pointer);
}

} // namespace

InflateReader::~InflateReader() {
    largeFree(decompressor);
    largeFree(window);
    largeFree(inputBuffer);
}

bool InflateReader::init() {
    if (decompressor == nullptr) {
        decompressor = fastAlloc(sizeof(tinfl_decompressor));
    }
    if (window == nullptr) {
        window = static_cast<uint8_t*>(largeAlloc(WINDOW_SIZE));
    }
    if (inputBuffer == nullptr) {
        inputBuffer = static_cast<uint8_t*>(fastAlloc(INPUT_BUFFER_SIZE));
    }
    return decompressor != nullptr && window != nullptr && inputBuffer != nullptr;
}

bool InflateReader::begin(ByteInput* source) {
    input = source;
    inputPosition = 0;
    inputSize = 0;
    inputEnded = false;
    windowOffset = 0;
    pending = nullptr;
    pendingEnd = nullptr;
    position = 0;
    finished = false;
    error = false;
    if (decompressor == nullptr || !input->rewind() || !fillInput()) {
        error = true;
        return false;
    }
    tinfl_init(asDecompressor(decompressor));
    if (inputSize >= 2 && inputBuffer[0] == 0x1F && inputBuffer[1] == 0x8B) {
        streamFormat = Format::Gzip;
        if (!skipGzipHeader()) {
            error = true;
            return false;
        }
    } else if (inputSize >= 2 && (inputBuffer[0] & 0x0F) == 8 && ((inputBuffer[0] << 8) | inputBuffer[1]) % 31 == 0) {
        streamFormat = Format::Zlib;
    } else {
        streamFormat = Format::Raw;
    }
    return true;
}

bool InflateReader::fillInput() {
    if (inputEnded) {
        return false;
    }
    if (inputPosition < inputSize) {
        memmove(inputBuffer, inputBuffer + inputPosition, inputSize - inputPosition);
    }
    inputSize -= inputPosition;
    inputPosition = 0;
    const int count = input->read(inputBuffer + inputSize, INPUT_BUFFER_SIZE - inputSize);
    if (count < 0) {
        error = true;
        inputEnded = true;
        return false;
    }
    if (count == 0) {
        inputEnded = true;
        return inputSize > 0;
    }
    inputSize += static_cast<size_t>(count);
    return true;
}

int InflateReader::getInputByte() {
    if (inputPosition >= inputSize && !fillInput()) {
        return -1;
    }
    return inputBuffer[inputPosition++];
}

bool InflateReader::skipGzipHeader() {
    uint8_t header[10];
    for (unsigned char& byte : header) {
        const int value = getInputByte();
        if (value < 0) {
            return false;
        }
        byte = static_cast<uint8_t>(value);
    }
    if (header[2] != 8) {
        return false;
    }
    const uint8_t flags = header[3];
    if (flags & 0x04) {
        const int low = getInputByte();
        const int high = getInputByte();
        if (low < 0 || high < 0) {
            return false;
        }
        for (int i = 0; i < (low | (high << 8)); i++) {
            if (getInputByte() < 0) {
                return false;
            }
        }
    }
    for (const uint8_t flag : { static_cast<uint8_t>(0x08), static_cast<uint8_t>(0x10) }) {
        if (flags & flag) {
            int value;
            do {
                value = getInputByte();
            } while (value > 0);
            if (value < 0) {
                return false;
            }
        }
    }
    if (flags & 0x02) {
        getInputByte();
        getInputByte();
    }
    return true;
}

bool InflateReader::refill() {
    if (finished || error) {
        return false;
    }
    if (streamFormat == Format::Raw) {
        if (inputPosition >= inputSize) {
            inputSize = 0;
            inputPosition = 0;
            if (!fillInput()) {
                finished = true;
                return false;
            }
        }
        pending = inputBuffer + inputPosition;
        pendingEnd = inputBuffer + inputSize;
        inputPosition = inputSize;
        return true;
    }

    while (true) {
        if (inputPosition >= inputSize && !inputEnded) {
            fillInput();
        }
        size_t inBytes = inputSize - inputPosition;
        size_t outBytes = WINDOW_SIZE - windowOffset;
        mz_uint32 flags = inputEnded ? 0 : TINFL_FLAG_HAS_MORE_INPUT;
        if (streamFormat == Format::Zlib) {
            flags |= TINFL_FLAG_PARSE_ZLIB_HEADER;
        }
        const tinfl_status status = tinfl_decompress(
            asDecompressor(decompressor),
            inputBuffer + inputPosition,
            &inBytes,
            window,
            window + windowOffset,
            &outBytes,
            flags
        );
        inputPosition += inBytes;
        if (outBytes > 0) {
            pending = window + windowOffset;
            pendingEnd = pending + outBytes;
            windowOffset = (windowOffset + outBytes) & (WINDOW_SIZE - 1);
            if (status == TINFL_STATUS_DONE) {
                finished = true;
            }
            return true;
        }
        if (status == TINFL_STATUS_DONE) {
            finished = true;
            return false;
        }
        if (status < TINFL_STATUS_DONE) {
            error = true;
            return false;
        }
        if (status == TINFL_STATUS_NEEDS_MORE_INPUT && inputEnded) {
            error = true;
            return false;
        }
    }
}

size_t InflateReader::read(uint8_t* buffer, size_t size) {
    size_t done = 0;
    while (done < size) {
        if (pending == pendingEnd && !refill()) {
            break;
        }
        size_t count = static_cast<size_t>(pendingEnd - pending);
        if (count > size - done) {
            count = size - done;
        }
        memcpy(buffer + done, pending, count);
        pending += count;
        done += count;
    }
    position += done;
    return done;
}

bool InflateReader::skip(size_t count) {
    while (count > 0) {
        if (pending == pendingEnd && !refill()) {
            return false;
        }
        size_t available = static_cast<size_t>(pendingEnd - pending);
        if (available > count) {
            available = count;
        }
        pending += available;
        position += available;
        count -= available;
    }
    return true;
}

} // namespace maps
