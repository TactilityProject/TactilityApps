// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

constexpr int TILE_SIZE = 256;
constexpr int MAX_SOURCE_ZOOM = 14;
constexpr int MAX_VIEW_ZOOM = 19;
constexpr double MAX_LATITUDE = 85.05112878;

struct TileKey {
    int z;
    int x;
    int y;

    bool operator==(const TileKey& other) const { return z == other.z && x == other.x && y == other.y; }
};

struct TileRange {
    int z;
    int minX;
    int minY;
    int maxX;
    int maxY;

    int64_t count() const { return static_cast<int64_t>(maxX - minX + 1) * (maxY - minY + 1); }
};

struct BoundingBox {
    double west;
    double south;
    double east;
    double north;
};

/** World pixel position at a zoom level, where the whole world is TILE_SIZE * 2^z pixels wide. */
double lonToWorldX(double lon, int z);
double latToWorldY(double lat, int z);
double worldXToLon(double x, int z);
double worldYToLat(double y, int z);

TileKey tileAt(double lat, double lon, int z);

/** Tiles covering a bounding box at one zoom level. The box must not cross the antimeridian. */
TileRange tileRange(const BoundingBox& box, int z);

int64_t tileCount(const BoundingBox& box, int minZoom, int maxZoom);

/** Ground metres per pixel at a latitude and zoom level. */
double metresPerPixel(double lat, int z);

/** The tile at zoom z that contains the given tile, z must not exceed tile.z. */
TileKey ancestorOf(const TileKey& tile, int z);

bool isValidTile(const TileKey& tile);

/** The tile at x, wrapped around the antimeridian. */
int wrapTileX(int x, int z);

} // namespace maps
