// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/TileMath.h"

#include <cstddef>
#include <cstdint>

namespace maps {

class ByteWriter;

/**
 * Sizes and last use of the rendered tiles in the SD card cache of one style, so the cache stays under a size
 * cap by deleting the least recently used tiles first. Kept in cache/<style-id>/index.bin.
 */
class CacheIndex {
public:
    static constexpr uint64_t DEFAULT_CAP_BYTES = 200ull * 1000 * 1000;
    /** Eviction stops at this share of the cap, so it does not run after every write. */
    static constexpr int LOW_WATER_PERCENT = 90;

    CacheIndex() = default;
    ~CacheIndex();
    CacheIndex(const CacheIndex&) = delete;
    CacheIndex& operator=(const CacheIndex&) = delete;

    bool init(int maxEntries, uint64_t capBytes = DEFAULT_CAP_BYTES);
    void clear();

    /** Adds or updates a tile and marks it as just used. */
    void put(const TileKey& tile, uint32_t size);
    void touch(const TileKey& tile);
    void erase(const TileKey& tile);
    bool contains(const TileKey& tile) const;

    uint64_t totalBytes() const { return total; }
    int size() const { return count; }
    bool isFull() const { return count >= capacity; }
    bool isOverCap() const { return total > cap || isFull(); }

    /**
     * The least recently used tile while the cache is above the low water mark, or while it is over the cap or
     * the entry limit. Returns false when nothing needs to go.
     */
    bool nextEviction(TileKey& tile);

    void save(ByteWriter& out) const;
    bool load(const uint8_t* data, size_t size);

    /** Set once the index matches the files on the card. */
    bool isComplete() const { return complete; }
    void setComplete(bool value) { complete = value; }
    bool isDirty() const { return dirty; }
    void setClean() { dirty = false; }

private:
    struct Entry {
        TileKey tile;
        uint32_t size;
        uint32_t lastUse;
    };

    int find(const TileKey& tile) const;

    Entry* entries = nullptr;
    int count = 0;
    int capacity = 0;
    uint64_t total = 0;
    uint64_t cap = DEFAULT_CAP_BYTES;
    uint32_t clock = 0;
    bool evicting = false;
    bool complete = false;
    bool dirty = false;
};

} // namespace maps
