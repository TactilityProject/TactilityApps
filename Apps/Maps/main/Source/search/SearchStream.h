// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/ByteInput.h"
#include "../mvt/MvtDecoder.h"
#include "../route/GrowArray.h"
#include "AddressMatch.h"
#include "ExternalSort.h"
#include "QueryParse.h"
#include "ScratchFiles.h"
#include "SearchExtract.h"
#include "SearchIndex.h"
#include "SearchRegion.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** A z14 tile of the region being indexed. */
struct StreamTile {
    int32_t x;
    int32_t y;
};

/** Gives the builder a tile's original vector tile (.pbf), one at a time. */
class StreamTileSource {
public:
    virtual ~StreamTileSource() = default;
    /** The tile's bytes, or nullptr when it cannot be read. Stays valid until the next call. */
    virtual ByteInput* open(int32_t x, int32_t y) = 0;
    /** Every tile was read: the decoder and the tile buffers are not needed any more (the device frees them). */
    virtual void tilesDone() {}
};

/** What the builder does now, for the progress line ("Building search index N/M"). */
struct StreamProgress {
    uint32_t done;
    uint32_t total;
};

struct StreamBuildOptions {
    RegionConfig config;
    /** The buffer of the sorted runs (each sort; at most two sorts hold memory at once). */
    uint32_t sortBufferBytes = 64 * 1024;
    /** Called between steps; returning false cancels the build (scratch files are removed). */
    bool (*keepGoing)(const StreamProgress& progress, void* context) = nullptr;
    void* context = nullptr;
};

/** Measurements of one build. */
struct StreamBuildStats {
    RegionStats region;
    SearchBuildStats index;
    /** Time per phase in ms: tiles, points, clusters, matching, addresses, name groups and records, keys, file. */
    uint32_t phaseMs[8];
    /** The slowest tile's extraction and its matching, in ms, and which tile. */
    uint32_t slowestExtractMs;
    StreamTile slowestExtractTile;
    uint32_t slowestMatchMs;
    StreamTile slowestMatchTile;
    /** Bytes written to scratch files (runs and extracts), and the most memory a sort held. */
    uint64_t scratchBytes;
    size_t sortPeakBytes;
    /** The builder's own tables at their largest (lines, clusters, names, labels, settlements, tiles). */
    size_t tableBytes;
    /** The house matcher's arrays at their largest (one tile, or one block of a dense tile). */
    size_t matcherPeakBytes;
};

/**
 * Builds a search index (.sidx) for a set of z14 tiles with little memory (M6, built on the device in idle time). It
 * makes the same file as SearchRegionBuilder + SearchIndexWriter (byte for byte; the host check compares them), but
 * keeps only small tables in memory and puts everything that grows with the number of points, house numbers and keys
 * through sorted runs on the card (ExternalSorter):
 *
 *  1. tiles: each tile is extracted (TileExtractor); its named lines and owned house numbers go to two scratch files,
 *     its points into a sort. Duplicate lines are found among the tile and its neighbours already read; a house number
 *     belongs to the tile it lies in (a copy in another tile's buffer is skipped), or, outside the region, to the first
 *     tile that has it;
 *  2. points: duplicates (label, name, position) dropped, the first one kept; label counts for the class pairs;
 *  3. street clusters: union-find over same-name lines whose vertex cells (150 m) touch, through a sort of the cells;
 *  4. matching: per tile, its houses with the lines of the tile and its neighbours and the neighbours' houses for the
 *     vote (AddressMatcher), written as address records;
 *  5. addresses: per street record, sorted by number; text numbers into the overflow strings;
 *  6. name groups, point records and keys through sorts; 7. the file: header, tables and the sections, then a rename.
 *
 * Memory: the decoder's pools while extracting (about 260 KB), one tile's extract, two sort buffers, and per region
 * about 12 bytes per line, 64 per street cluster and the street names. Platform free; files through ScratchFiles.
 */
class SearchStreamBuilder {
public:
    SearchStreamBuilder();
    ~SearchStreamBuilder();
    SearchStreamBuilder(const SearchStreamBuilder&) = delete;
    SearchStreamBuilder& operator=(const SearchStreamBuilder&) = delete;

    /**
     * Builds the index of the tiles (in this order; the host tool sorts by x, then y) into outPath, written as
     * temporaryPath and renamed. scratchDir must exist; the builder's files inside it are removed at the end. The
     * decoder must be initialised (init()); it is used with its POI pools and shrunk again afterwards.
     */
    bool build(ScratchFiles& files, const char* scratchDir, const StreamTile* tiles, uint32_t tileCount, StreamTileSource& source,
               MvtDecoder& decoder, const char* outPath, const char* temporaryPath, const StreamBuildOptions& options);

    const StreamBuildStats& stats() const { return statistics; }
    const char* error() const { return errorText; }
    bool cancelled() const { return wasCancelled; }

private:
    struct Impl;
    Impl* impl = nullptr;
    StreamBuildStats statistics = {};
    char errorText[160] = {};
    bool wasCancelled = false;
};

/** The house number's grammar as the region statistics count it (SearchRegionBuilder uses the same). */
struct HouseNumberStyle {
    bool parsed;
    bool plain;
    bool slash;
    bool secondSmaller;
    HouseNumber number;
};
HouseNumberStyle houseNumberStyle(const char* text);
/** The street number a house votes with: the second part of "x/y" when slashSecond, else the first; -1 for none. */
int32_t houseVoteNumber(const char* text, bool slashSecond);

} // namespace maps
