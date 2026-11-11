// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../render/PreparedLabels.h"
#include "../tiles/TileMath.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * The label sections of the source tiles drawn lately, read by the map loop to place labels. They come from the
 * render lanes: copied out of an older prepared tile with labels inside as it is drawn (TileRenderer's label hook),
 * or, for split tiles (version 5 on), read from the tile's .prl file on request: a lane that draws a split tile asks
 * for its labels, a lane loads them (TileWorker::loadLabels). House numbers are read only while the view wants them.
 * Entries live in PSRAM, at most MAX_BYTES together; the least recently used goes first. One z14 entry serves the
 * z15-z19 tiles drawn from it.
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
    /**
     * Called on a render lane: keeps a copy of a tile's label section, replacing an older one. numbers tells whether it
     * has the tile's house numbers; region is the region the .prl file was read from (-1 for an older tile).
     */
    void put(const TileKey& source, const uint8_t* section, size_t size, int count, bool numbers = true, int region = -1);
    /**
     * For a split tile's labels: takes a largeAlloc block it then owns (freed on failure too), laid out as LabelCells
     * followed by size bytes of records.
     */
    void adopt(const TileKey& source, uint8_t* block, size_t size, int count, bool numbers, int region);

    struct Request {
        TileKey tile;
        int region;
        bool numbers;
    };
    /** Asks for the labels of a split tile drawn from region: queued unless they are stored already. */
    void request(const TileKey& source, int region);
    /** For the lanes: the next labels to read, false when none are wanted. The tile counts as being read until done(). */
    bool takeRequest(Request& out);
    /** The lane finished reading a request (stored or failed). */
    void done(const TileKey& source);
    /** Set by the map from its zoom: house numbers are read with the labels, and stored labels without them read again. */
    void setNumbersWanted(bool wanted) { numbersWanted = wanted; }
    bool wantsNumbers() const { return numbersWanted; }
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
            visit(entries[index].bytes, entries[index].size, entries[index].count, entries[index].cells);
            // Placed without house numbers for now, they are read in the background
            if (numbersWanted && !entries[index].numbers && entries[index].region >= 0) {
                queue(entries[index].tile, entries[index].region);
            }
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
        bool numbers;
        int region;
        /** nullptr for an older tile's section; else the start of the block, with the records after it. */
        LabelCells* cells;
    };
    static constexpr int MAX_REQUESTS = 16;
    static constexpr int MAX_READING = 4;

    int find(const TileKey& tile) const;
    void evict(int index);
    /** With the lock held. */
    void queue(const TileKey& tile, int region);
    void store(const TileKey& source, uint8_t* records, LabelCells* cells, size_t size, int count, bool numbers, int region);
    bool isReading(const TileKey& tile) const;

    Request requests[MAX_REQUESTS] {};
    int requestCount = 0;
    Request reading[MAX_READING] {};
    int readingCount = 0;
    std::atomic<bool> numbersWanted { false };

    Entry entries[MAX_ENTRIES] {};
    int entryCount = 0;
    size_t bytesUsed = 0;
    uint32_t useCounter = 0;
    std::atomic<uint32_t> changes { 0 };
    SemaphoreHandle_t mutex = nullptr;
};

} // namespace maps
