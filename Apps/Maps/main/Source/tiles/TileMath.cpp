// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileMath.h"

#include <algorithm>
#include <cmath>

namespace maps {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double EARTH_CIRCUMFERENCE_M = 40075016.686;

double worldSize(int z) {
    return static_cast<double>(TILE_SIZE) * static_cast<double>(1u << z);
}

} // namespace

double lonToWorldX(double lon, int z) {
    return (lon + 180.0) / 360.0 * worldSize(z);
}

double latToWorldY(double lat, int z) {
    lat = std::clamp(lat, -MAX_LATITUDE, MAX_LATITUDE);
    const double rad = lat * PI / 180.0;
    return (1.0 - std::log(std::tan(rad) + 1.0 / std::cos(rad)) / PI) / 2.0 * worldSize(z);
}

double worldXToLon(double x, int z) {
    return x / worldSize(z) * 360.0 - 180.0;
}

double worldYToLat(double y, int z) {
    const double n = PI - 2.0 * PI * y / worldSize(z);
    return 180.0 / PI * std::atan(std::sinh(n));
}

TileKey tileAt(double lat, double lon, int z) {
    const int last = (1 << z) - 1;
    const int x = std::clamp(static_cast<int>(std::floor(lonToWorldX(lon, z) / TILE_SIZE)), 0, last);
    const int y = std::clamp(static_cast<int>(std::floor(latToWorldY(lat, z) / TILE_SIZE)), 0, last);
    return { z, x, y };
}

TileRange tileRange(const BoundingBox& box, int z) {
    const TileKey topLeft = tileAt(box.north, box.west, z);
    const TileKey bottomRight = tileAt(box.south, box.east, z);
    return {
        .z = z,
        .minX = std::min(topLeft.x, bottomRight.x),
        .minY = std::min(topLeft.y, bottomRight.y),
        .maxX = std::max(topLeft.x, bottomRight.x),
        .maxY = std::max(topLeft.y, bottomRight.y),
    };
}

int64_t tileCount(const BoundingBox& box, int minZoom, int maxZoom) {
    int64_t total = 0;
    for (int z = minZoom; z <= maxZoom; z++) {
        total += tileRange(box, z).count();
    }
    return total;
}

double metresPerPixel(double lat, int z) {
    return EARTH_CIRCUMFERENCE_M * std::cos(lat * PI / 180.0) / worldSize(z);
}

TileKey ancestorOf(const TileKey& tile, int z) {
    const int shift = tile.z - z;
    return { z, tile.x >> shift, tile.y >> shift };
}

bool isValidTile(const TileKey& tile) {
    if (tile.z < 0 || tile.z > 30) {
        return false;
    }
    const int count = 1 << tile.z;
    return tile.x >= 0 && tile.x < count && tile.y >= 0 && tile.y < count;
}

int wrapTileX(int x, int z) {
    const int count = 1 << z;
    x %= count;
    return x < 0 ? x + count : x;
}

} // namespace maps
