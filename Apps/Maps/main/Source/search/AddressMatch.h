// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../route/GrowArray.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** Settlement classes for the village rule; locality is never one (M6 study, gt_village.py). */
enum class PlaceClass : uint8_t { City, Town, Village, Hamlet, IsolatedDwelling, Suburb, Quarter, Neighbourhood, Count };

/** The scale of a place class in metres: a village 800 m away counts as much as a hamlet 300 m away. */
float placeClassScale(PlaceClass placeClass);

/** How a house number found its place in the index. */
enum class AddressTier : uint8_t { None, Street, LowStreet };

/** The result for one house number. */
struct AddressMatch {
    /** The street's name id (the caller's), UINT32_MAX for none. */
    uint32_t street;
    float streetM;
    AddressTier tier;
    /** Up to three settlements, best first (name ids, UINT32_MAX for none), for village-style numbers. */
    uint32_t villages[3];
};

/**
 * Matches house numbers to streets and settlements (M6, the study's measured rules, docs/M6-search-study.md):
 *  - the nearest named street line (point to segment, in metres) within STREET_M is the street, within LOW_STREET_M a
 *    low-confidence one, never farther;
 *  - street-run vote: when a second street is within the nearest + VOTE_SLACK_M, neighbours within VOTE_M with the same
 *    parity and numbers at most 4 apart vote for their street; the street changes only with strictly more votes;
 *  - village style numbers get the best three settlements within 3 km by distance divided by the class scale.
 * Coordinates are global z14 units (4096 per tile). One matcher works on a tile and its buffer, then is cleared; its
 * arrays keep their memory for the next tile.
 */
class AddressMatcher {
public:
    static constexpr float STREET_M = 50.0f;
    static constexpr float LOW_STREET_M = 100.0f;
    static constexpr float VOTE_M = 60.0f;
    static constexpr float VOTE_SLACK_M = 12.0f;
    static constexpr float PLACE_M = 3000.0f;

    /** Starts a tile; metres per unit at its latitude. */
    void begin(double metresPerUnit);
    /**
     * Leaves out the segments that lie wholly outside this box (global units) until the next begin(). For one tile:
     * its houses and the neighbours' that vote lie within VOTE_M of it and look LOW_STREET_M around them, so segments
     * farther out can never be a candidate; leaving them out saves memory and changes no result.
     */
    void clip(int32_t x0, int32_t y0, int32_t x1, int32_t y1);
    /** Clips to a tile (x, y at z14) and the reach of its houses at this latitude, with a margin. */
    void clipToTile(int32_t tileX, int32_t tileY);
    /** A named street line (its points, global units). */
    void addLine(uint32_t name, const int32_t* xy, size_t pointCount);
    void addPlace(uint32_t name, PlaceClass placeClass, int32_t x, int32_t y);
    /** A house number: its street number for the vote (-1: none), and whether it looks village style (plain). */
    uint32_t addHouse(int32_t x, int32_t y, int32_t number, bool villageStyle);
    /** Works out every house's match. False when memory ran out. */
    bool run();
    const AddressMatch& result(uint32_t house) const { return results[house]; }
    size_t houseCount() const { return houses.size(); }
    /** Frees the memory. */
    void release();
    /** The memory its arrays hold. */
    size_t allocated() const {
        return segments.allocated() + houses.allocated() + places.allocated() + segmentGrid.allocated() + houseGrid.allocated() +
            placeGrid.allocated() + results.allocated() + candidates.allocated();
    }

private:
    struct Segment {
        int32_t ax, ay, bx, by;
        uint32_t name;
    };
    struct CellItem {
        uint32_t cell;
        uint32_t item;
    };
    struct House {
        int32_t x, y;
        int32_t number;
        bool villageStyle;
    };
    struct Place {
        int32_t x, y;
        uint32_t name;
        PlaceClass placeClass;
    };
    /** The nearest distinct streets of a house, nearest first. */
    struct Candidate {
        uint32_t name;
        float metres;
    };
    static constexpr int CANDIDATES = 4;

    uint32_t cellOf(int32_t x, int32_t y, int32_t cellUnits) const;
    void nearestStreets(const House& house, Candidate* out, int& count) const;
    void villagesFor(const House& house, uint32_t* out) const;
    template <typename Visit>
    void visitCells(const GrowArray<CellItem>& grid, int32_t cellUnits, int32_t x, int32_t y, int32_t radiusUnits, Visit visit) const;

    double metresPerUnit = 1.0;
    bool clipping = false;
    int32_t clipX0 = 0, clipY0 = 0, clipX1 = 0, clipY1 = 0;
    int32_t segmentCell = 256;
    int32_t houseCell = 128;
    int32_t placeCell = 8192;
    GrowArray<Segment> segments;
    GrowArray<House> houses;
    GrowArray<Place> places;
    GrowArray<CellItem> segmentGrid;
    GrowArray<CellItem> houseGrid;
    GrowArray<CellItem> placeGrid;
    GrowArray<AddressMatch> results;
    GrowArray<Candidate> candidates;
};

} // namespace maps
