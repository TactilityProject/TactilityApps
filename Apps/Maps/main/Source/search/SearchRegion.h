// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../route/GrowArray.h"
#include "AddressMatch.h"
#include "SearchExtract.h"
#include "SearchIndex.h"
#include "TextFold.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** Metres per z14 unit at a tile row (fractional, e.g. the middle of a region's rows). */
double metresPerUnitAtTileY(double tileY);

/** An open-addressing table from a 64-bit hash to a caller's index; the caller's callback decides equality. */
class HashIndex {
public:
    static constexpr uint32_t NONE = UINT32_MAX;
    void clear();
    void release();
    /** The value stored under the hash for which eq(value) is true, or NONE. */
    template <typename Eq>
    uint32_t find(uint64_t hash, Eq eq) const {
        if (values.size() == 0) {
            return NONE;
        }
        const size_t mask = values.size() - 1;
        for (size_t i = static_cast<size_t>(hash) & mask; values[i] != NONE; i = (i + 1) & mask) {
            if (hashes[i] == hash && eq(values[i])) {
                return values[i];
            }
        }
        return NONE;
    }
    /** Adds a value (the caller has checked it is not there). */
    void insert(uint64_t hash, uint32_t value);
    bool failed() const { return values.failed() || hashes.failed(); }

private:
    void put(uint64_t hash, uint32_t value);
    GrowArray<uint32_t> values;
    GrowArray<uint64_t> hashes;
    size_t used = 0;
};

/**
 * The search keys of a name as the study makes them (fold.search_keys and suffix_keys): the folded name (transliterated
 * and original-folded for Greek and Cyrillic), name:latin, name_en when useEn, then the text from each later word of at
 * least 3 characters. Duplicates are left out; the order is fixed. Also the name's sort key. Shared by the in-memory
 * builder and the streaming one, so both make the same keys.
 */
class KeyMaker {
public:
    /** Makes the keys of a name; false when out of memory. */
    bool make(const char* name, const char* latin, const char* en, bool useEn, Translit profile);
    /** The sort key (name groups are ordered by it). */
    const char* sortKey() const { return &pool[0]; }
    uint32_t count() const { return static_cast<uint32_t>(offsets.size()); }
    const char* key(uint32_t index) const { return &pool[offsets[index]]; }
    /** Only the sort key, without the other keys. */
    static void sortKeyOf(const char* name, Translit profile, char* out, size_t outSize);
    void release() {
        pool.release();
        offsets.release();
    }

private:
    static constexpr size_t BUFFER = 2048;
    bool has(const char* key) const;
    void add(const char* key);
    char scratch[5][BUFFER] = {};
    GrowArray<char> pool;
    GrowArray<uint32_t> offsets;
};

struct RegionConfig {
    /** Transliteration of Greek and Cyrillic names (the study's PROFILE map: Ukrainian for kyiv, Serbian for belgrade, else Russian). */
    Translit profile = Translit::Russian;
    /** Metres per z14 unit; 0 takes it from the middle row of the region's tiles. */
    double metresPerUnit = 0.0;
    /**
     * Named lines of class path, track, aerialway, ferry and rail take part in the house matching. The study's counts
     * leave them out; with them 88.3 % of Bratislava's numbers get the street of OSM's addr:street instead of 87.2 %
     * (host/search/gt_cpp.py, 12 Nov 2026), and the brief asks for named paths and pedestrian streets.
     */
    bool matchPathLines = true;
    /** For "x/y" numbers of a conscription/street region the vote compares the street number (second part). */
    bool voteSlashSecond = true;
    /** The share of plain numbers from which a region is in village mode, and of x/y numbers with a smaller second part for slash_second. */
    double villageShare = 0.30;
    double slashSecondShare = 0.80;
};

struct RegionStats {
    uint32_t tiles = 0;
    // What was extracted, deduplicated across tiles (for comparison with the study's counts)
    uint32_t places = 0;
    uint32_t namedPois = 0;
    uint32_t unnamedPois = 0;
    uint32_t otherNamed = 0;
    uint32_t junkPoisSkipped = 0;
    uint32_t streetNames = 0;
    uint32_t lines = 0;
    uint32_t clusters = 0;
    uint32_t refOnlyLines = 0;
    uint32_t settlements = 0;
    uint32_t houses = 0;
    // House number grammar
    uint32_t plainNumbers = 0;
    double plainShare = 0;
    bool villageMode = false;
    uint32_t slashNumbers = 0;
    uint32_t slashSecondSmaller = 0;
    double slashShare = 0;
    bool slashSecond = false;
    uint32_t textNumbers = 0;
    // The path of each house number: street within 50 m, street 50 to 100 m, village only, none
    uint32_t pathStreet50 = 0;
    uint32_t pathStreet100 = 0;
    uint32_t pathVillage = 0;
    uint32_t pathNone = 0;
    /** Houses that also went under a settlement while having a street (counted in the street paths above). */
    uint32_t alsoVillage = 0;
    uint32_t pseudoStreets = 0;
    uint32_t droppedAddresses = 0;
    uint32_t addresses = 0;
};

/** One house number and where it went, for dumps and checks. */
struct HouseView {
    const char* text;
    int32_t x;
    int32_t y;
    /** The matched street name, nullptr for none. */
    const char* street;
    AddressTier tier;
    float streetM;
    /** The settlement the number also went under, nullptr for none. */
    const char* village;
    bool villageStyle;
};

/**
 * The region-level extraction and matching, host sized (everything in RAM): addTile() for every z14 tile, then
 * buildSearchInput(). The SearchInput points into the builder, which must outlive its use. The device will stream
 * the same steps; this is the reference for them.
 *
 * What it does (M6 study rules, docs/M6-search-study.md):
 *  - points are deduplicated by (label, name, x, y), lines by (name, points), house numbers by (text, x, y);
 *  - street clusters: same-name lines whose vertices are in the same or adjacent 150 m cell (union-find), representative
 *    vertex nearest to the centroid, bounding box;
 *  - house numbers: matched per tile with AddressMatcher, with the lines of the neighbour tiles up to 160 m outside the
 *    tile and the neighbouring houses up to 60 m outside (for the street-run vote; their own results are not used). The
 *    matcher's street ids are cluster ids, so the match is already the cluster nearest to the house;
 *  - slash_second (street number is the second part of "x/y") when at least RegionConfig::slashSecondShare of the
 *    region's "x/y" numbers have a smaller second part (the study's rule); the vote uses that part as the street number;
 *  - village mode when at least RegionConfig::villageShare of all numbers are plain ("245"); then plain numbers also go
 *    under their best settlement (matcher's villages[0]) as an address with ADDRESS_VILLAGE under a street record named
 *    like the place, with a one-point cluster; with a street within 50 m they are under the street too, within 50 to
 *    100 m under it with ADDRESS_LOW;
 *  - keys exactly as the study (search_keys and suffix_keys with TextFold), name_en only for places, water, peaks, parks.
 */
class SearchRegionBuilder {
public:
    SearchRegionBuilder();
    ~SearchRegionBuilder();
    SearchRegionBuilder(const SearchRegionBuilder&) = delete;
    SearchRegionBuilder& operator=(const SearchRegionBuilder&) = delete;

    /** Extracts one tile and merges it. */
    bool addTile(MvtDecoder& decoder, ByteInput* input, int tileX, int tileY);
    /** Clusters, matching, keys. Call once, after the last tile. */
    bool buildSearchInput(const RegionConfig& config);

    const SearchInput& input() const { return searchInput; }
    const RegionStats& stats() const { return statistics; }
    const char* error() const { return errorText; }

    size_t houseCount() const { return houses.size(); }
    HouseView house(size_t index) const;

private:
    struct Pt {
        uint32_t label;
        uint32_t name;
        uint32_t latin;
        uint32_t en;
        int32_t x, y;
        uint8_t settlement;
        uint8_t useEn;
    };
    struct Ln {
        uint32_t nameId;
        uint32_t latin;
        uint32_t first;
        uint32_t count;
        uint32_t cluster;
        int32_t minX, minY, maxX, maxY;
        uint8_t pathClass;
    };
    struct Hs {
        uint32_t text;
        int32_t x, y;
        uint32_t tile;
        /** Its index in the extract of the tile that owns it: the order of the tile's houses (the streaming build's too). */
        uint32_t local;
        uint8_t inside;
        uint8_t tier;
        uint32_t cluster;
        uint32_t village;
        float streetM;
        uint8_t villageStyle;
    };
    struct TileRef {
        int x, y;
    };
    struct Cluster {
        uint32_t street;
        int32_t repX, repY, minX, minY, maxX, maxY;
    };
    struct Street {
        uint32_t nameId;
        uint32_t latin;
        uint32_t en;
        uint32_t firstCluster;
        uint32_t clusterCount;
        uint8_t useEn;
    };
    struct Settlement {
        uint32_t point;
        uint32_t nameId;
        uint8_t placeClass;
    };
    struct Pseudo {
        uint32_t street;
        uint32_t settlement;
    };
    struct AddressRec {
        uint32_t cluster;
        uint32_t house;
        uint8_t flags;
    };
    struct KeyRef {
        uint32_t sortKey;
        uint32_t first;
        uint32_t count;
    };

    uint32_t copyText(const char* text);
    uint32_t internLabel(const char* label);
    uint32_t internName(const char* name);
    const char* textAt(uint32_t offset) const { return offset == EXTRACT_NONE ? nullptr : &text[offset]; }
    bool mergeTile(const ExtractedTile& tile, uint32_t tileIndex);
    bool makeClusters(double metresPerUnit);
    bool matchHouses(const RegionConfig& config, double metresPerUnit, bool villageMode, bool slashSecond);
    bool assemble(const RegionConfig& config);
    KeyRef makeKeys(const char* name, const char* latin, const char* en, bool useEn, Translit profile);
    bool anyFailed() const;
    bool fail(const char* message);

    GrowArray<char> text;
    HashIndex labelIndex;
    GrowArray<uint32_t> labelOffsets;
    HashIndex nameIndex;
    GrowArray<uint32_t> nameOffsets;
    HashIndex pointIndex;
    HashIndex lineIndex;
    HashIndex houseIndex;
    HashIndex tileIndex;
    GrowArray<Pt> points;
    GrowArray<Ln> lines;
    GrowArray<int32_t> linePoints;
    GrowArray<Hs> houses;
    GrowArray<TileRef> tiles;
    GrowArray<Cluster> clusters;
    GrowArray<Street> streets;
    GrowArray<Settlement> settlements;
    GrowArray<Pseudo> pseudos;
    GrowArray<AddressRec> addressRecs;
    GrowArray<uint32_t> settlementPseudo;
    GrowArray<uint32_t> nameStreet;
    // Keys
    GrowArray<char> keyPool;
    GrowArray<uint32_t> keyOffsets;
    GrowArray<KeyRef> pointKeys;
    GrowArray<KeyRef> streetKeys;
    HashIndex keyShare;
    // The writer's input
    GrowArray<SearchPointInput> pointInputs;
    GrowArray<SearchStreetInput> streetInputs;
    GrowArray<SearchClusterInput> clusterInputs;
    GrowArray<SearchAddressInput> addressInputs;
    GrowArray<const char*> keyPointers;
    GrowArray<const char*> categoryNames;
    GrowArray<const char*> categoryKeys;
    SearchInput searchInput = {};
    RegionStats statistics;
    ExtractedTile tileBuffer;
    TileExtractor extractor;
    AddressMatcher matcher;
    char errorText[160] = {};
    KeyMaker keyMaker;
    char categoryScratch[2048] = {};
};

} // namespace maps
