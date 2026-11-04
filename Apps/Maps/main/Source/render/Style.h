// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/Schema.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** Draw stages in painting order. Each stage draws one kind of feature of one layer. */
enum class Stage : uint8_t {
    Landcover,
    Landuse,
    Park,
    Water,
    Waterway,
    Aeroway,
    Boundary,
    Building,
    RoadCasing,
    RoadFill,
    Count
};

Layer stageLayer(Stage stage);

/** Changes whenever the style changes, so tiles rendered with an older style are rendered again. */
constexpr uint32_t STYLE_VERSION = 4;

/** Pattern numbers for DrawStyle::pattern, all with period 4 so they join across tile edges. */
constexpr uint8_t PATTERN_NONE = 0;
constexpr uint8_t PATTERN_DOTS = 1;
constexpr uint8_t PATTERN_LINES = 2;
constexpr uint8_t PATTERN_DIAGONAL = 3;
constexpr uint8_t PATTERN_DOTS_OFFSET = 4;

/** True where a pixel belongs to the pattern, in the coordinates of the whole map so tiles stay aligned. */
bool patternBit(uint8_t pattern, int x, int y);

struct Palette {
    uint16_t background;
    uint16_t placeholder;
    uint16_t water;
    uint16_t wood;
    uint16_t grass;
    uint16_t farmland;
    uint16_t wetland;
    uint16_t sand;
    uint16_t ice;
    uint16_t rock;
    uint16_t residential;
    uint16_t commercial;
    uint16_t industrial;
    uint16_t cemetery;
    uint16_t hospital;
    uint16_t school;
    uint16_t pitch;
    uint16_t military;
    uint16_t park;
    uint16_t building;
    uint16_t aerodrome;
    uint16_t runway;
    uint16_t boundary;
    uint16_t motorway;
    uint16_t motorwayCasing;
    uint16_t trunk;
    uint16_t trunkCasing;
    uint16_t primary;
    uint16_t primaryCasing;
    uint16_t secondary;
    uint16_t secondaryCasing;
    uint16_t minor;
    uint16_t minorCasing;
    uint16_t path;
    uint16_t rail;
    uint16_t pier;
    uint16_t text;
    uint16_t halo;
    uint16_t marker;
    /** Draw only pure black and white, using patterns instead of greys. */
    bool binary;
};

const Palette& colorPalette();
/** High contrast palette for monochrome and grayscale displays. */
const Palette& monochromePalette();
/** Pure black and white for 1-bit displays, where anti-aliased greys would turn invisible. */
const Palette& binaryPalette();

/** A short, 8.3-safe name for the style and palette, the SD card cache folder of their rendered tiles. */
void styleCacheId(const Palette& palette, char* out, size_t size);

struct DrawStyle {
    uint16_t color;
    uint8_t opacity;
    /** Filled polygon when 0, otherwise a stroke of this width in pixels. */
    float lineWidth;
    /** PATTERN_* number for pattern fills, PATTERN_NONE for a solid fill. */
    uint8_t pattern;
};

/**
 * Picks how a feature is drawn in a stage. zoom is the zoom level of the rendered tile, which can be above the
 * zoom level of the source data. Returns false when the feature is not drawn in this stage.
 */
bool styleFor(const FeatureInfo& info, Stage stage, int zoom, const Palette& palette, DrawStyle& style);

} // namespace maps
