// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/TileMath.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * The label sections of the source tiles drawn lately, copied out by the render lanes as they draw a prepared tile
 * (TileRenderer's label hook) and read by the map loop to place labels. Entries live in PSRAM, at most
 * MAX_BYTES together; the least recently used goes first. One z14 entry serves the z15-z19 tiles drawn from it.
 */
class LabelStore {
public:
    static constexpr int MAX_ENTRIES = 48;
    /** 512 KB: v4 sections of dense city tiles (POIs and house numbers) are over 100 KB each (device test, 10 Nov 2026). */
    static constexpr size_t MAX_BYTES = 512 * 1024;
    /** The largest section kept: a third of the store, so a view's tiles fit together. */
    static constexpr size_t MAX_SECTION_BYTES = MAX_BYTES / 3;

    bool begin();
    void end();
    /** Called on a render lane: keeps a copy of a tile's label section, replacing an older one. */
    void put(const TileKey& source, const uint8_t* section, size_t size, int count);
    /**
     * Calls visit with the section of a source tile while holding the store's lock; false when it is not stored. The
     * section must not be used after visit returns.
     */
    template <typename Visit>
    bool with(const TileKey& source, Visit&& visit) {
        if (mutex == nullptr) {
            return false;
        }
        xSemaphoreTake(mutex, portMAX_DELAY);
        const int index = find(source);
        if (index >= 0) {
            entries[index].lastUse = ++useCounter;
            visit(entries[index].bytes, entries[index].size, entries[index].count);
        }
        xSemaphoreGive(mutex);
        return index >= 0;
    }
    /** Changes whenever a section was added, so the map knows when to place its labels again. */
    uint32_t generation() const { return changes.load(); }

private:
    struct Entry {
        TileKey tile;
        uint8_t* bytes;
        size_t size;
        int count;
        uint32_t lastUse;
    };

    int find(const TileKey& tile) const;
    void evict(int index);

    Entry entries[MAX_ENTRIES] {};
    int entryCount = 0;
    size_t bytesUsed = 0;
    uint32_t useCounter = 0;
    std::atomic<uint32_t> changes { 0 };
    SemaphoreHandle_t mutex = nullptr;
};

} // namespace maps
