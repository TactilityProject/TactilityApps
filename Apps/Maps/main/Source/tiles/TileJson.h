// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "TileMath.h"

#include <cstddef>

namespace maps {

constexpr const char* TILEJSON_URL = "https://tiles.openfreemap.org/planet";
constexpr const char* USER_AGENT = "TactilityMaps/0.0.1 (+https://github.com/TactilityProject/TactilityApps)";

/**
 * Reads the first tile URL template of a TileJSON document, like ".../planet/20261025_080012_pt/{z}/{x}/{y}.pbf",
 * and the version, which is the path segment before "/{z}".
 */
bool parseTileJson(const char* text, char* urlTemplate, size_t templateSize, char* version, size_t versionSize);

/** Fills {z}, {x} and {y} of a URL template. */
bool formatTileUrl(const char* urlTemplate, const TileKey& tile, char* out, size_t size);

} // namespace maps
