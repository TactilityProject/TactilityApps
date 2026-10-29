// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/TileMath.h"

#include <cstdint>

namespace maps {

/** Returns the rendered 256x256 pixels of a tile, or nullptr when it is not ready. */
using TileLookup = const uint16_t* (*)(const TileKey& tile, void* context);

/**
 * Copies the tiles that cover a view into it. centerX and centerY are world pixel coordinates at zoom.
 * Returns the number of tiles that were missing and drawn as placeholder.
 */
int composeView(uint16_t* view, int width, int height, double centerX, double centerY, int zoom, TileLookup lookup, void* context, uint16_t placeholder);

/**
 * Draws source scaled by scale into target, both width x height, so the source point fromX, fromY lands on
 * toX, toY. Nearest pixel, for live pinch zoom from the pixels on screen. Uncovered parts get fill.
 */
void scaleView(const uint16_t* source, uint16_t* target, int width, int height, float scale, float fromX, float fromY, float toX, float toY,
    uint16_t fill);

/** The whole zoom level nearest to zoom scaled by scale, within 0 and maxZoom. */
int snapZoom(int zoom, float scale, int maxZoom);

} // namespace maps
