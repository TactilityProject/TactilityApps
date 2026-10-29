// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "TileMath.h"

#include <cstdint>

namespace maps {

/** OpenFreeMap's limits for one offline download. They are not to be worked around by splitting an area. */
constexpr int64_t MAX_DOWNLOAD_TILES = 3000;
constexpr uint64_t MAX_DOWNLOAD_BYTES = 50ull * 1000 * 1000;
constexpr int MAX_PARALLEL_REQUESTS = 4;

struct DownloadPlan {
    BoundingBox box;
    int minZoom;
    int maxZoom;
    int64_t tiles;
    uint64_t estimatedBytes;
    /** False when even the lowest zoom levels do not fit the limits. */
    bool fits;
};

/** Typical compressed size of a tile at a zoom level, measured from real OpenFreeMap tiles. */
uint32_t typicalTileBytes(int z);

uint64_t estimateBytes(const BoundingBox& box, int minZoom, int maxZoom);

/** The plan with the highest max zoom, at most requestedMaxZoom, that fits the download limits. */
DownloadPlan planDownload(const BoundingBox& box, int requestedMaxZoom);

/**
 * The tile at a position in the download order: zoom levels from low to high, rows from top to bottom,
 * columns from left to right. The order never changes, so a download resumes from its done count.
 */
bool tileAtIndex(const DownloadPlan& plan, int64_t index, TileKey& tile);

} // namespace maps
