// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../platform/Platform.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace maps {

/**
 * A fixed-size array of trivially copyable values in pages of 4,096 values, in large memory. The route search's
 * node table and queue (up to 1 MB together) used to be single blocks: after one long search on the T-Deck the PSRAM
 * had 2.8 MB free but no block above 384 KB, and the next search found no memory (15 Nov 2026). Pages of 32-64 KB
 * fit between the tile cache's slots.
 */
template <typename T>
class PagedArray {
public:
    static constexpr uint32_t PAGE_SHIFT = 12;
    static constexpr uint32_t PAGE_SIZE = 1u << PAGE_SHIFT;
    static constexpr uint32_t MAX_PAGES = 256;

    PagedArray() = default;
    PagedArray(const PagedArray&) = delete;
    PagedArray& operator=(const PagedArray&) = delete;
    ~PagedArray() { release(); }

    /** Allocates room for count values (not cleared); false (and nothing kept) when the memory is not there. */
    bool allocate(uint32_t count) {
        release();
        const uint32_t pages = (count + PAGE_SIZE - 1) >> PAGE_SHIFT;
        if (pages > MAX_PAGES) {
            return false;
        }
        for (uint32_t i = 0; i < pages; i++) {
            page[i] = static_cast<T*>(largeAlloc(sizeof(T) * PAGE_SIZE));
            if (page[i] == nullptr) {
                pageCount = i;
                release();
                return false;
            }
        }
        pageCount = pages;
        size = count;
        return true;
    }

    void release() {
        for (uint32_t i = 0; i < pageCount; i++) {
            largeFree(page[i]);
            page[i] = nullptr;
        }
        pageCount = 0;
        size = 0;
    }

    /** Sets every byte of every value to the same value. */
    void fill(uint8_t byte) {
        for (uint32_t i = 0; i < pageCount; i++) {
            memset(static_cast<void*>(page[i]), byte, sizeof(T) * PAGE_SIZE);
        }
    }

    bool isAllocated() const { return pageCount > 0; }
    uint32_t count() const { return size; }
    T& operator[](uint32_t index) { return page[index >> PAGE_SHIFT][index & (PAGE_SIZE - 1)]; }
    const T& operator[](uint32_t index) const { return page[index >> PAGE_SHIFT][index & (PAGE_SIZE - 1)]; }

private:
    T* page[MAX_PAGES] {};
    uint32_t pageCount = 0;
    uint32_t size = 0;
};

} // namespace maps
