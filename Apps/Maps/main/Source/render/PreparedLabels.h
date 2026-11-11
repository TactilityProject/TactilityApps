// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/Schema.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Poi labels (format 4): labelClass holds the type id of PoiTypes.h, not a Value. HouseNumber labels: the name is the
 * house number, labelClass is 0 and rank -1.
 */
enum class LabelKind : uint8_t { Place, Road, Water, Waterway, Peak, Poi, HouseNumber };

struct LabelRecord {
    LabelKind kind;
    Value labelClass;
    /** -1 when absent */
    int32_t rank;
    const char* name;
    uint16_t nameLength;
    /** nullptr when the record has none */
    const char* nameLatin;
    uint16_t nameLatinLength;
    /** x, y pairs in the 2048 extent, valid until the next call of next() */
    const int32_t* points;
    int pointCount;
};

/** Walks the labels of a label section (its bytes after the section header). */
class LabelReader {
public:
    static constexpr int MAX_POINTS = 256;
    void begin(const uint8_t* bytes, size_t size, int count);
    /** False at the end or on damaged bytes. Lines with more points than MAX_POINTS are cut to MAX_POINTS. */
    bool next(LabelRecord& out);

private:
    const uint8_t* data = nullptr;
    size_t length = 0;
    size_t offset = 0;
    int remaining = 0;
    int32_t pointBuffer[2 * MAX_POINTS] {};
};

/**
 * Where the POI and house number records of a label file lie: they are sorted into an 8 x 8 grid of cells over the tile
 * (LABEL_CELL_UNITS each), so placement reads only the cells near the screen. The other labels (lines, places) come
 * first, unsorted. Offsets count from the start of the records; a cell is count records from its offset on.
 */
constexpr int LABEL_GRID = 8;
constexpr int LABEL_CELLS = LABEL_GRID * LABEL_GRID;
constexpr int LABEL_CELL_UNITS = 2048 / LABEL_GRID;
/** The index block of a .prl file, after its header: u32 general count, u32 general bytes, then for the POIs and for
 * the house numbers 64 x (u32 offset in its part, u32 count). */
constexpr size_t LABEL_INDEX_SIZE = 8 + 2 * LABEL_CELLS * 8;

struct LabelCells {
    uint32_t generalCount;
    uint32_t generalBytes;
    uint32_t poiOffset[LABEL_CELLS];
    uint32_t poiCount[LABEL_CELLS];
    uint32_t numberOffset[LABEL_CELLS];
    uint32_t numberCount[LABEL_CELLS];
};

/** The cell of a point in tile units (outside points go to the nearest cell). */
inline int labelCell(int32_t x, int32_t y) {
    const int column = x < 0 ? 0 : (x >= 2048 ? LABEL_GRID - 1 : x / LABEL_CELL_UNITS);
    const int row = y < 0 ? 0 : (y >= 2048 ? LABEL_GRID - 1 : y / LABEL_CELL_UNITS);
    return row * LABEL_GRID + column;
}

/**
 * Reads the index block of a .prl file for records laid out as part A (mainSize bytes) then, with numbers, part B.
 * False when it does not fit those sizes.
 */
bool readLabelCells(const uint8_t* index, uint32_t mainSize, uint32_t numberSize, bool numbers, LabelCells& out);

/** Finds the label section in an inflated prepared body. False when there is none (also for v2 tiles). */
bool findLabelSection(const uint8_t* body, size_t bodySize, const uint8_t*& section, size_t& sectionSize, int& count);

} // namespace maps
