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

/**
 * Where the screen samples the world: the world pixel at zoom of the screen point (sx, sy), measured from the
 * screen centre, is (centerX + a * sx + b * sy, centerY + c * sx + d * sy). A plain view is a = d = 1, b = c = 0;
 * a = d = 1 / scale magnifies by scale; a rotation fills in b and c.
 */
struct AffineView {
    double centerX;
    double centerY;
    int zoom;
    float a;
    float b;
    float c;
    float d;
};

/**
 * The view of a map magnified by scale around the screen point (anchorX, anchorY), which shows the world point at zoom
 * (worldX, worldY), turned by the angle whose cosine and sine are given (a screen offset turns by it into a world offset).
 */
AffineView scaledView(double worldX, double worldY, int zoom, float scale, float anchorX, float anchorY, int width, int height,
    float cosine = 1.0f, float sine = 0.0f);

/** Working memory of composeAffine, about 6 KB: too much for a task stack, so the caller keeps one (in PSRAM). */
struct AffineScratch {
    static constexpr int MAX_GRID = 12;
    static constexpr int MAX_WIDTH = 1024;

    /** How the pixels of one tile of the view are found: in the tile itself, in an ancestor, or in its four children. */
    struct Cell {
        const uint16_t* pixels;
        /** Children, top left, top right, bottom left, bottom right, when pixels is null. */
        const uint16_t* children[4];
        /** Where the tile lies in the ancestor's pixels, and how many levels up (0 for the tile itself). */
        int16_t originX;
        int16_t originY;
        uint8_t shift;
        bool hasChildren;
    };

    Cell cells[MAX_GRID * MAX_GRID];
    int16_t columns[MAX_WIDTH];
};

/**
 * Draws the view through an affine transform from rendered tiles, nearest pixel: for zoom animations and pinch, and
 * later rotation. A tile that is not ready is stood in for by a cached ancestor (magnified) or by its four children
 * (shrunk). lookup must return only tiles that are really rendered. Returns the number of tiles drawn as placeholder.
 * With bilinear, each pixel blends the four nearest source pixels: about four times the work, for a settled turned
 * view. rowBegin and rowEnd (exclusive, -1 for the last row) limit the rows drawn, so slow frames can go in strips.
 */
int composeAffine(uint16_t* view, int width, int height, const AffineView& transform, TileLookup lookup, void* context, uint16_t placeholder,
    AffineScratch& scratch, bool bilinear = false, int rowBegin = 0, int rowEnd = -1);

/** RGB565 from from to to by weight / 32 (0..32). */
uint16_t lerpRgb565(uint16_t from, uint16_t to, uint32_t weight);

} // namespace maps
