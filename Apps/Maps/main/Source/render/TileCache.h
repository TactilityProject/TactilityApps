// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/TileMath.h"

#include <cstdint>

namespace maps {

/** Rendered tiles in a fixed number of slots, the least recently used slot is reused first. */
class TileCache {
public:
    static constexpr int TILE_BYTES = TILE_SIZE * TILE_SIZE * 2;

    ~TileCache();

    /** Allocates up to maxSlots tiles, fewer when memory runs out. Returns the number of slots. */
    int init(int maxSlots);

    /** The pixels of a cached tile, marking it as recently used, or nullptr. Stale tiles are found too. */
    const uint16_t* find(const TileKey& tile);
    /** Whether a current (not stale) copy of the tile is cached, so it needs no drawing. */
    bool contains(const TileKey& tile) const;
    /** The pixels of a cached tile without marking it as used. */
    const uint16_t* peek(const TileKey& tile) const;

    /** A slot for a new tile, which is not findable until commit() and is not reused until commit() or release(). */
    uint16_t* reserve(const TileKey& tile);
    /** sourceZoom is the zoom level of the vector tile the pixels were drawn from. Replaces a stale copy. */
    void commit(const TileKey& tile, int sourceZoom);
    void release(const TileKey& tile);

    void clear();
    /**
     * Marks tiles drawn magnified from a lower zoom level as stale, after better data may have arrived. They stay
     * on screen until they are drawn again, so the map does not blink while a download stores tiles.
     */
    void dropMagnified();
    int slotCount() const { return count; }

private:
    struct Slot {
        TileKey key;
        uint32_t lastUse;
        bool valid;
        bool stale;
        bool reserved;
        int8_t sourceZoom;
        uint16_t* pixels;
    };

    Slot* slots = nullptr;
    int count = 0;
    uint32_t useCounter = 0;
};

} // namespace maps
