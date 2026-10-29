// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "TileMath.h"
#include "TilePaths.h"

#include <cstddef>

namespace maps {

/** The region that tiles fetched while browsing go to. */
constexpr const char* BROWSE_REGION = "browsed";

/** An original vector tile file on the card. */
struct TileFile {
    TileKey tile;
    int region;
    uint32_t size;
    char path[PATH_MAX_LENGTH];
};

/** Finds tile files in the region folders under <root>/maps. */
class TileStore {
public:
    static constexpr int MAX_REGIONS = 16;
    /** How many zoom levels a tile may be magnified from an ancestor when its own file is missing. */
    static constexpr int MAX_OVERZOOM = 6;

    void setRoot(const char* rootPath);
    bool addRegion(const char* name);
    void removeRegion(const char* name);
    /** Lists the region folders under <root>/maps and reads their tile set versions. */
    int scanRegions();
    int regionCount() const { return count; }
    const char* regionName(int index) const { return regions[index]; }
    /** The tile set version the region was downloaded from, empty when unknown. */
    const char* regionVersion(int index) const { return versions[index]; }
    void setRegionVersion(const char* name, const char* version);
    /**
     * The area and zoom levels a region's tiles can be in, from its meta.json. find() only looks for a tile in
     * regions that can have it: each look on a FAT card costs tens of milliseconds. A zoom range with
     * minZoom > maxZoom stands for all source zoom levels. Regions without bounds are always looked in, and so is
     * the browsed region, whose box earlier versions reset.
     */
    void setRegionBounds(const char* name, const BoundingBox& box, int minZoom, int maxZoom);
    /** Whether a region can have a tile. */
    bool mayHave(int index, const TileKey& tile) const;

    /**
     * Finds the file to draw a tile from: the tile itself, or the nearest ancestor that exists, never above
     * MAX_SOURCE_ZOOM. Returns false when there is none.
     */
    bool find(const TileKey& target, TileKey& source, char* path, size_t pathSize) const;
    /** Like find(), also naming the region and the file size. */
    bool find(const TileKey& target, TileFile& file) const;

    const char* root() const { return rootPath; }

private:
    char rootPath[PATH_MAX_LENGTH] = "";
    char regions[MAX_REGIONS][REGION_NAME_MAX + 1] {};
    char versions[MAX_REGIONS][48] {};
    struct Bounds {
        bool known;
        int8_t minZoom;
        int8_t maxZoom;
        BoundingBox box;
    };
    Bounds bounds[MAX_REGIONS] {};
    int count = 0;
};

} // namespace maps
