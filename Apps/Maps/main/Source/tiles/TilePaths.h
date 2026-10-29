// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "TileMath.h"

#include <cstddef>

namespace maps {

constexpr size_t PATH_MAX_LENGTH = 128;
constexpr size_t REGION_NAME_MAX = 8;

/** Writes "<root>/maps/<region>/<z>/<x>/<y>.pbf". Returns false when the path does not fit. */
bool tilePath(char* out, size_t size, const char* root, const char* region, const TileKey& tile);

/** Writes "<root>/maps/<region>/<z>/<x>/<y>.prt", the prepared copy next to the original. */
bool preparedPath(char* out, size_t size, const char* root, const char* region, const TileKey& tile);
/** Writes "<root>/maps/<region>/<z>/<x>/<y>.ptm", where a prepared tile is written before it is renamed. */
bool preparedTempPath(char* out, size_t size, const char* root, const char* region, const TileKey& tile);

/** Writes "<root>/maps/<region>/<z>/<x>", the folder of a tile column. */
bool tileFolderPath(char* out, size_t size, const char* root, const char* region, const TileKey& tile);

/** Writes "<root>/cache/<style>/<z>/<x>/<y>" for a rendered tile. style names the style and palette, see styleCacheId(). */
bool cachePath(char* out, size_t size, const char* root, const char* style, const TileKey& tile);

bool cacheFolderPath(char* out, size_t size, const char* root, const char* style, const TileKey& tile);

/** Whether a path lies inside <root>/cache, the only place the cache may delete files. */
bool isInsideCache(const char* path, const char* root);

/** A region name is 1 to 8 characters of a-z, 0-9, '_' or '-', so it is a valid 8.3 folder name. */
bool isValidRegionName(const char* name);

} // namespace maps
