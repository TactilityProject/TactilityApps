// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../route/GrowArray.h"
#include "AddressMatch.h"
#include "QueryParse.h"
#include "SearchExtract.h"
#include "SearchIndex.h"
#include "TextFold.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** Metres per z14 unit at a tile row (fractional, e.g. the middle of a region's rows). */
double metresPerUnitAtTileY(double tileY);

/**
 * Cuts a polyline (x, y pairs) to the rectangle x0..x1, y0..y1 (inclusive): emit(xy, pointCount) is called for each
 * piece inside, in order. A line wholly inside is emitted as it is; the points where a line crosses the border are
 * rounded to whole units. Used by both index builders for the lines of margin tiles (they must cut alike).
 * piece is scratch space.
 */
template <typename Emit>
void clipPolyline(const int32_t* xy, size_t count, int32_t x0, int32_t y0, int32_t x1, int32_t y1, GrowArray<int32_t>& piece, Emit emit) {
    if (count < 2) {
        return;
    }
    int32_t lx0 = xy[0], ly0 = xy[1], lx1 = xy[0], ly1 = xy[1];
    for (size_t i = 1; i < count; i++) {
        lx0 = xy[2 * i] < lx0 ? xy[2 * i] : lx0;
        lx1 = xy[2 * i] > lx1 ? xy[2 * i] : lx1;
        ly0 = xy[2 * i + 1] < ly0 ? xy[2 * i + 1] : ly0;
        ly1 = xy[2 * i + 1] > ly1 ? xy[2 * i + 1] : ly1;
    }
    if (lx1 < x0 || lx0 > x1 || ly1 < y0 || ly0 > y1) {
        return;
    }
    if (lx0 >= x0 && lx1 <= x1 && ly0 >= y0 && ly1 <= y1) {
        emit(xy, count);
        return;
    }
    piece.clear();
    auto add = [&](int32_t x, int32_t y) {
        const size_t n = piece.size();
        if (n >= 2 && piece[n - 2] == x && piece[n - 1] == y) {
            return;
        }
        piece.push(x);
        piece.push(y);
    };
    auto flush = [&]() {
        if (piece.size() >= 4) {
            emit(piece.begin(), piece.size() / 2);
        }
        piece.clear();
    };
    auto rounded = [](double v, int32_t lo, int32_t hi) {
        auto r = static_cast<int32_t>(v >= 0 ? v + 0.5 : v - 0.5);
        return r < lo ? lo : (r > hi ? hi : r);
    };
    for (size_t i = 0; i + 1 < count; i++) {
        const double ax = xy[2 * i];
        const double ay = xy[2 * i + 1];
        const double dx = static_cast<double>(xy[2 * i + 2]) - ax;
        const double dy = static_cast<double>(xy[2 * i + 3]) - ay;
        const double p[4] = { -dx, dx, -dy, dy };
        const double q[4] = { ax - x0, x1 - ax, ay - y0, y1 - ay };
        double t0 = 0.0;
        double t1 = 1.0;
        bool visible = true;
        for (int k = 0; k < 4 && visible; k++) {
            if (p[k] == 0.0) {
                visible = q[k] >= 0.0;
                continue;
            }
            const double r = q[k] / p[k];
            if (p[k] < 0.0) {
                visible = r <= t1;
                t0 = r > t0 ? r : t0;
            } else {
                visible = r >= t0;
                t1 = r < t1 ? r : t1;
            }
        }
        if (!visible || t0 > t1) {
            flush();
            continue;
        }
        if (t0 > 0.0) {
            flush();
            add(rounded(ax + t0 * dx, x0, x1), rounded(ay + t0 * dy, y0, y1));
        } else {
            add(xy[2 * i], xy[2 * i + 1]);
        }
        if (t1 < 1.0) {
            add(rounded(ax + t1 * dx, x0, x1), rounded(ay + t1 * dy, y0, y1));
            flush();
        } else {
            add(xy[2 * i + 2], xy[2 * i + 3]);
        }
    }
    flush();
}

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
    /** A value could not be added (no memory to grow): the index misses it. */
    bool failed() const { return full || values.failed() || hashes.failed(); }

private:
    void put(uint64_t hash, uint32_t value);
    GrowArray<uint32_t> values;
    GrowArray<uint64_t> hashes;
    size_t used = 0;
    bool full = false;
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

/** The house number's grammar as the region statistics count it (both builders use it, so their files match). */
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
