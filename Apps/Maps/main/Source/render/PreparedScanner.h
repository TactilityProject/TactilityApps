// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/TileStore.h"

#include <cstdint>

namespace maps {

/**
 * Walks the region folders a little at a time and reports original tiles whose prepared copy is missing or was
 * made by an older format version, so idle time converts the tiles already on the card.
 */
class PreparedScanner {
public:
    /**
     * Called for a tile that needs converting; split is true for a tile prepared by a version with labels inside, which
     * is split into its new files. Returning false stops the step, it is looked at again next time.
     */
    using StaleCallback = bool (*)(const TileKey& tile, int region, bool split, void* context);

    ~PreparedScanner();

    /** Starts over from the first region, after tiles arrived or the format changed. */
    void restart();
    bool isComplete() const { return complete; }

    /** Looks at one folder of tiles. Returns false once every region was looked at. */
    bool step(const TileStore& store, StaleCallback callback, void* context);

    /** Whether the prepared copy at path, and the files made with it, belong to this original and are current. */
    static bool isCurrent(const char* preparedPath, const TileKey& tile, uint32_t sourceSize);
    /**
     * The parts (PART_* of PreparedTile.h) a tile needs, from the header of its prepared copy at path. split tells
     * whether that copy is an older one with labels inside.
     */
    static uint8_t partsNeeded(const char* preparedPath, const TileKey& tile, uint32_t sourceSize, bool* split = nullptr);

private:
    static constexpr int MAX_COLUMNS = 4096;

    bool listColumns(const TileStore& store);

    int region = 0;
    int zoom = 0;
    int column = 0;
    int columnCount = -1;
    /** Originals of the current folder already looked at. */
    int fileSkip = 0;
    int* columns = nullptr;
    bool complete = false;
};

} // namespace maps
