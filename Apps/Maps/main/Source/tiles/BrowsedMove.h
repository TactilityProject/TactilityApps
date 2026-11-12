// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "TileMath.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** A browsed tile a download wants: moved into the region, from another source version (download it), or failed. */
enum class BrowsedMove : uint8_t { Moved, Stale, Failed };

/**
 * Moves a tile from the region "browsed" into region: two renames on the same card, the prepared copy (.prt) first,
 * then the original (.pbf). Power lost between them leaves the region with the prepared copy only and the original
 * still browsed; calling this again for the tile (the download does, when it resumes) finishes the move.
 *
 * The tile's source version is the one in its prepared copy's header, or browsedVersion when it has none. A tile from
 * another version than version is not moved (Stale): the download fetches it fresh and then removes the old copy.
 * An empty version on either side counts as a match. size receives the original's size in bytes.
 * makeFolder creates one folder (mkdir); it is passed in so the PC tests run the same code.
 */
BrowsedMove moveBrowsedFiles(const char* root, const char* region, const TileKey& tile, const char* version,
    const char* browsedVersion, uint64_t& size, bool (*makeFolder)(const char* path));

/** Deletes a stale browsed tile (prepared copy, then original); returns the original's size, 0 when it was not there. */
uint64_t removeBrowsedFiles(const char* root, const TileKey& tile);

} // namespace maps
