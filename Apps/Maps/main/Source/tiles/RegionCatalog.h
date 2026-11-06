// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "TileMath.h"

#include <cstddef>
#include <cstdint>

namespace maps {

struct CatalogRegion {
    int parent;          // -1 for none
    int level;
    const char* name;    // NOT NUL-terminated, nameLength bytes
    int nameLength;
    BoundingBox box;     // degrees (from TileMath.h)
    int childCount;
};

/** Reads a region catalog file held in memory; the buffer must outlive the catalog. */
class RegionCatalog {
public:
    /** Checks the header and that every record's offsets lie inside the buffer. */
    bool open(const uint8_t* data, size_t size);
    bool isOpen() const;
    int count() const;
    /** Region i, 0 <= i < count(). */
    CatalogRegion region(int index) const;
    /** Copies the region's name into out as a NUL-terminated string, cut to fit. */
    void copyName(int index, char* out, size_t size) const;
    /** The children of parent (-1: the top level) into out in file order, at most max; returns how many. */
    int children(int parent, int* out, int max) const;
    /** Whether the point lies inside the region's outline (even-odd over all its rings). */
    bool contains(int index, double lat, double lon) const;
    /** The deepest region whose outline contains the point; among equal levels the one with the smaller box. -1 if none. */
    int smallestContaining(double lat, double lon) const;
    /**
     * The tiles at zoom z that touch the region's outline: a tile counts when its centre is inside, or when an outline
     * edge passes through it. Calls visit(tile, context) for each, row by row from north to south, west to east,
     * until visit returns false. Returns the number of tiles visited. With visit == nullptr it only counts, and stops
     * counting once the count passes limit (returns limit + 1 then).
     */
    int64_t forEachTile(int index, int z, bool (*visit)(const TileKey& tile, void* context), void* context, int64_t limit = INT64_MAX) const;

private:
    const uint8_t* record(int index) const;
    const uint8_t* outlineStart(int index, size_t& end) const;

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    int count_ = 0;
    uint32_t outlineOffset_ = 0;
    uint32_t nameOffset_ = 0;
    bool open_ = false;
};

} // namespace maps
