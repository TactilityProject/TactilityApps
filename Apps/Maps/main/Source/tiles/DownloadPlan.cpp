// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadPlan.h"

namespace maps {

namespace {

// Mean compressed bytes per tile of 290 OpenFreeMap tiles (planet 20261025) from Budapest, Vienna, London,
// Split and rural Hungary. Dense city centres are up to twice as large, the countryside much smaller.
constexpr uint32_t TYPICAL_TILE_BYTES[MAX_SOURCE_ZOOM + 1] = {
    48000, 124000, 700000, 509000, 360000, 292000, 198000, 225000, 179000, 158000, 90000, 66000, 72000, 60000, 136000
};

} // namespace

uint32_t typicalTileBytes(int z) {
    if (z < 0) {
        return 0;
    }
    return TYPICAL_TILE_BYTES[z > MAX_SOURCE_ZOOM ? MAX_SOURCE_ZOOM : z];
}

uint64_t estimateBytes(const BoundingBox& box, int minZoom, int maxZoom) {
    uint64_t total = 0;
    for (int z = minZoom; z <= maxZoom; z++) {
        total += static_cast<uint64_t>(tileRange(box, z).count()) * typicalTileBytes(z);
    }
    return total;
}

DownloadPlan planDownload(const BoundingBox& box, int requestedMaxZoom) {
    DownloadPlan plan { box, 0, 0, 0, 0, false };
    const int top = requestedMaxZoom < MAX_SOURCE_ZOOM ? requestedMaxZoom : MAX_SOURCE_ZOOM;
    for (int maxZoom = top; maxZoom >= 0; maxZoom--) {
        const int64_t tiles = tileCount(box, 0, maxZoom);
        const uint64_t bytes = estimateBytes(box, 0, maxZoom);
        if (tiles <= MAX_DOWNLOAD_TILES && bytes <= MAX_DOWNLOAD_BYTES) {
            plan.maxZoom = maxZoom;
            plan.tiles = tiles;
            plan.estimatedBytes = bytes;
            plan.fits = true;
            return plan;
        }
    }
    plan.tiles = tileCount(box, 0, 0);
    plan.estimatedBytes = estimateBytes(box, 0, 0);
    return plan;
}

bool tileAtIndex(const DownloadPlan& plan, int64_t index, TileKey& tile) {
    if (index < 0) {
        return false;
    }
    for (int z = plan.minZoom; z <= plan.maxZoom; z++) {
        const TileRange range = tileRange(plan.box, z);
        const int64_t count = range.count();
        if (index < count) {
            const int64_t width = range.maxX - range.minX + 1;
            tile = { z, range.minX + static_cast<int>(index % width), range.minY + static_cast<int>(index / width) };
            return true;
        }
        index -= count;
    }
    return false;
}

} // namespace maps
