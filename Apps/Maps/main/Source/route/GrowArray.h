// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../platform/Platform.h"

#include <cstddef>
#include <cstring>

namespace maps {

/**
 * A growing array of trivially copyable values in large memory (PSRAM on the device). It never throws: a failed
 * growth sets failed() and drops the value.
 */
template <typename T>
class GrowArray {
public:
    GrowArray() = default;
    ~GrowArray() { largeFree(data); }
    GrowArray(const GrowArray&) = delete;
    GrowArray& operator=(const GrowArray&) = delete;

    void clear() {
        length = 0;
        error = false;
    }
    /** Clears and frees the memory. */
    void release() {
        largeFree(data);
        data = nullptr;
        capacity = 0;
        clear();
    }
    bool reserve(size_t count) {
        if (count <= capacity) {
            return true;
        }
        size_t next = capacity < 64 ? 64 : capacity;
        while (next < count) {
            next *= 2;
        }
        auto* grown = static_cast<T*>(largeAlloc(next * sizeof(T)));
        if (grown == nullptr) {
            error = true;
            return false;
        }
        if (length > 0) {
            memcpy(static_cast<void*>(grown), data, length * sizeof(T));
        }
        largeFree(data);
        data = grown;
        capacity = next;
        return true;
    }
    /** Room for count values, filled with zero bytes. */
    bool resize(size_t count) {
        if (!reserve(count)) {
            return false;
        }
        if (count > length) {
            memset(static_cast<void*>(data + length), 0, (count - length) * sizeof(T));
        }
        length = count;
        return true;
    }
    void push(const T& value) {
        if (length < capacity || reserve(length + 1)) {
            data[length++] = value;
        }
    }
    void pop() {
        if (length > 0) {
            length--;
        }
    }
    T& operator[](size_t index) { return data[index]; }
    const T& operator[](size_t index) const { return data[index]; }
    T* begin() { return data; }
    T* end() { return data + length; }
    const T* begin() const { return data; }
    const T* end() const { return data + length; }
    size_t size() const { return length; }
    size_t allocated() const { return capacity * sizeof(T); }
    bool failed() const { return error; }

private:
    T* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool error = false;
};

} // namespace maps
