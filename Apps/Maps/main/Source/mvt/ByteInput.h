// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace maps {

/** A sequential byte source that can start over from the beginning. */
class ByteInput {
public:
    virtual ~ByteInput() = default;
    /** Returns the number of bytes read, 0 at the end and a negative value on error. */
    virtual int read(uint8_t* buffer, size_t size) = 0;
    virtual bool rewind() = 0;
};

class FileInput final : public ByteInput {
    FILE* file = nullptr;

public:
    ~FileInput() override { close(); }

    bool open(const char* path) {
        close();
        file = fopen(path, "rb");
        return file != nullptr;
    }

    void close() {
        if (file != nullptr) {
            fclose(file);
            file = nullptr;
        }
    }

    int read(uint8_t* buffer, size_t size) override {
        if (file == nullptr) {
            return -1;
        }
        const size_t count = fread(buffer, 1, size, file);
        if (count == 0 && ferror(file)) {
            return -1;
        }
        return static_cast<int>(count);
    }

    bool rewind() override { return file != nullptr && fseek(file, 0, SEEK_SET) == 0; }
};

class MemoryInput final : public ByteInput {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t position = 0;

public:
    MemoryInput() = default;
    MemoryInput(const uint8_t* data, size_t size) : data(data), size(size) {}

    void reset(const uint8_t* newData, size_t newSize) {
        data = newData;
        size = newSize;
        position = 0;
    }

    int read(uint8_t* buffer, size_t count) override {
        const size_t left = size - position;
        const size_t n = count < left ? count : left;
        memcpy(buffer, data + position, n);
        position += n;
        return static_cast<int>(n);
    }

    bool rewind() override {
        position = 0;
        return true;
    }
};

/** A whole file in one buffer that grows as needed, so a tile is read from the card only once. */
class FileBuffer {
    uint8_t* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;

public:
    FileBuffer() = default;
    ~FileBuffer();
    FileBuffer(const FileBuffer&) = delete;
    FileBuffer& operator=(const FileBuffer&) = delete;

    bool load(const char* path);
    const uint8_t* bytes() const { return data; }
    size_t size() const { return length; }

    /** For readers that fill the buffer themselves: room for size bytes, then setSize() with what was read. */
    bool reserve(size_t size);
    uint8_t* writable() { return data; }
    void setSize(size_t size) { length = size <= capacity ? size : 0; }
    void clear() { length = 0; }
};

} // namespace maps
