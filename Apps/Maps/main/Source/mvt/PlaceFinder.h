// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ByteInput.h"
#include "InflateReader.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** A settlement found in a tile: its name, class rank (0 city, 1 town, 2 village) and distance in tile units. */
struct PlaceName {
    char name[48];
    int rank;
    double distance;
};

/** Finds the settlement nearest to a point of a vector tile, reading the tile as a stream. */
class PlaceFinder {
public:
    PlaceFinder() = default;
    ~PlaceFinder();
    PlaceFinder(const PlaceFinder&) = delete;
    PlaceFinder& operator=(const PlaceFinder&) = delete;
    /** Allocates the decompressor and tables once (malloc is fine here, about 40 KB together). */
    bool init();
    /**
     * Looks through the tile's place layer for city, town and village points. (x, y) is the point in tile units of
     * that layer's extent (0..extent, usually 4096; scale it yourself from the fraction the caller gives: see fx, fy).
     * fx and fy are the point as a fraction of the tile, 0..1 from the top left corner.
     * The best place is the one with the lowest score = distance in tile fractions * (1 + rank), so a city a little
     * farther away wins over a village next to the point. Returns false when the tile has no such place or is damaged.
     */
    bool nearest(ByteInput* tile, double fx, double fy, PlaceName& out);

private:
    static constexpr int VALUE_TABLE_SIZE = 2048;

    struct LayerInfo {
        bool found;
        size_t start;
        size_t length;
        int32_t extent;
        int16_t classKey;
        int16_t nameKey;
        /** "name:latin", preferred: region titles are shown with the system font, which has only Latin letters. */
        int16_t latinKey;
        uint32_t valueCount;
    };

    bool readVarint(uint64_t& value);
    bool skipField(uint32_t wireType);
    bool scanTile(ByteInput* tile);
    bool scanValue(size_t valueEnd, uint32_t index);
    bool findFeatures(ByteInput* tile, double fx, double fy);
    bool readFeature(size_t featureEnd, double fx, double fy);
    bool readGeometry(size_t geometryEnd, int32_t& px, int32_t& py, bool& hasPoint);
    bool readName(ByteInput* tile, uint32_t index, PlaceName& out);
    uint8_t valueRank(uint32_t index) const;

    InflateReader reader;
    LayerInfo layer {};
    uint32_t* valueOffsets = nullptr;
    uint16_t* valueLengths = nullptr;
    uint8_t* valueRanks = nullptr;
    double bestScore = 0.0;
    double bestDistance = 0.0;
    int bestRank = -1;
    uint32_t bestName = 0;
};

} // namespace maps
