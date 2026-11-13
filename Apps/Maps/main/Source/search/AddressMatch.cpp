// SPDX-License-Identifier: GPL-3.0-or-later
#include "AddressMatch.h"

#include <algorithm>
#include <cmath>

namespace maps {

namespace {

constexpr uint32_t NONE = UINT32_MAX;

float segmentDistance(float px, float py, float ax, float ay, float bx, float by) {
    const float dx = bx - ax;
    const float dy = by - ay;
    const float lengthSquared = dx * dx + dy * dy;
    float t = lengthSquared > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / lengthSquared : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = ax + t * dx - px;
    const float ey = ay + t * dy - py;
    return std::sqrt(ex * ex + ey * ey);
}

/** Insertion sort for the few candidates of a lookup (std::sort on a 3-entry array upsets GCC's bounds check). */
template <typename T, typename Less>
void smallSort(T* items, int count, Less less) {
    for (int i = 1; i < count; i++) {
        T item = items[i];
        int j = i - 1;
        while (j >= 0 && less(item, items[j])) {
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = item;
    }
}

int32_t floorDiv(int32_t value, int32_t by) {
    return value >= 0 ? value / by : -((-value + by - 1) / by);
}

} // namespace

float placeClassScale(PlaceClass placeClass) {
    switch (placeClass) {
        case PlaceClass::City:
            return 3000.0f;
        case PlaceClass::Town:
            return 1500.0f;
        case PlaceClass::Village:
            return 800.0f;
        case PlaceClass::Hamlet:
            return 300.0f;
        case PlaceClass::IsolatedDwelling:
            return 150.0f;
        case PlaceClass::Suburb:
            return 700.0f;
        case PlaceClass::Quarter:
            return 500.0f;
        case PlaceClass::Neighbourhood:
            return 400.0f;
        default:
            return 1.0f;
    }
}

void AddressMatcher::begin(double unitMetres) {
    metresPerUnit = unitMetres;
    // Cells of about the search radius: a lookup visits 3 x 3 cells
    segmentCell = std::max<int32_t>(16, static_cast<int32_t>(LOW_STREET_M / metresPerUnit));
    houseCell = std::max<int32_t>(16, static_cast<int32_t>(VOTE_M / metresPerUnit));
    placeCell = std::max<int32_t>(16, static_cast<int32_t>(PLACE_M / metresPerUnit));
    clipping = false;
    segments.clear();
    houses.clear();
    places.clear();
    segmentGrid.clear();
    houseGrid.clear();
    placeGrid.clear();
    results.clear();
}

void AddressMatcher::clip(int32_t x0, int32_t y0, int32_t x1, int32_t y1) {
    clipping = true;
    clipX0 = x0;
    clipY0 = y0;
    clipX1 = x1;
    clipY1 = y1;
}

void AddressMatcher::clipToTile(int32_t tileX, int32_t tileY) {
    // The vote's reach plus the search radius, 20 m and two units to spare
    const auto margin = static_cast<int32_t>((VOTE_M + LOW_STREET_M + 20.0) / metresPerUnit) + 2;
    clip(tileX * 4096 - margin, tileY * 4096 - margin, tileX * 4096 + 4095 + margin, tileY * 4096 + 4095 + margin);
}

void AddressMatcher::release() {
    segments.release();
    houses.release();
    places.release();
    segmentGrid.release();
    houseGrid.release();
    placeGrid.release();
    results.release();
    candidates.release();
}

uint32_t AddressMatcher::cellOf(int32_t x, int32_t y, int32_t cellUnits) const {
    // Cells far apart may share a key: lookups measure every item they find, so that only costs time
    const auto cx = static_cast<uint32_t>(floorDiv(x, cellUnits)) & 0xFFFF;
    const auto cy = static_cast<uint32_t>(floorDiv(y, cellUnits)) & 0xFFFF;
    return cx << 16 | cy;
}

void AddressMatcher::addLine(uint32_t name, const int32_t* xy, size_t pointCount) {
    for (size_t i = 1; i < pointCount; i++) {
        const Segment segment { xy[2 * i - 2], xy[2 * i - 1], xy[2 * i], xy[2 * i + 1], name };
        if (clipping && (std::max(segment.ax, segment.bx) < clipX0 || std::min(segment.ax, segment.bx) > clipX1 ||
                         std::max(segment.ay, segment.by) < clipY0 || std::min(segment.ay, segment.by) > clipY1)) {
            continue;
        }
        const auto index = static_cast<uint32_t>(segments.size());
        segments.push(segment);
        // In every cell its box touches (segments are short in vector tiles; long ones cover a few cells)
        const int32_t x0 = floorDiv(std::min(segment.ax, segment.bx), segmentCell);
        const int32_t x1 = floorDiv(std::max(segment.ax, segment.bx), segmentCell);
        const int32_t y0 = floorDiv(std::min(segment.ay, segment.by), segmentCell);
        const int32_t y1 = floorDiv(std::max(segment.ay, segment.by), segmentCell);
        for (int32_t cx = x0; cx <= x1 && cx - x0 < 64; cx++) {
            for (int32_t cy = y0; cy <= y1 && cy - y0 < 64; cy++) {
                segmentGrid.push({ cellOf(cx * segmentCell, cy * segmentCell, segmentCell), index });
            }
        }
    }
}

void AddressMatcher::addPlace(uint32_t name, PlaceClass placeClass, int32_t x, int32_t y) {
    placeGrid.push({ cellOf(x, y, placeCell), static_cast<uint32_t>(places.size()) });
    places.push({ x, y, name, placeClass });
}

uint32_t AddressMatcher::addHouse(int32_t x, int32_t y, int32_t number, bool villageStyle) {
    const auto index = static_cast<uint32_t>(houses.size());
    houseGrid.push({ cellOf(x, y, houseCell), index });
    houses.push({ x, y, number, villageStyle });
    return index;
}

template <typename Visit>
void AddressMatcher::visitCells(const GrowArray<CellItem>& grid, int32_t cellUnits, int32_t x, int32_t y, int32_t radiusUnits, Visit visit) const {
    const int32_t x0 = floorDiv(x - radiusUnits, cellUnits);
    const int32_t x1 = floorDiv(x + radiusUnits, cellUnits);
    const int32_t y0 = floorDiv(y - radiusUnits, cellUnits);
    const int32_t y1 = floorDiv(y + radiusUnits, cellUnits);
    for (int32_t cx = x0; cx <= x1; cx++) {
        for (int32_t cy = y0; cy <= y1; cy++) {
            const uint32_t key = cellOf(cx * cellUnits, cy * cellUnits, cellUnits);
            const CellItem probe { key, 0 };
            const CellItem* first =
                std::lower_bound(grid.begin(), grid.end(), probe, [](const CellItem& a, const CellItem& b) { return a.cell < b.cell; });
            for (const CellItem* item = first; item != grid.end() && item->cell == key; item++) {
                visit(item->item);
            }
        }
    }
}

void AddressMatcher::nearestStreets(const House& house, Candidate* out, int& count) const {
    count = 0;
    const auto radius = static_cast<int32_t>(LOW_STREET_M / metresPerUnit) + 1;
    const auto px = static_cast<float>(house.x);
    const auto py = static_cast<float>(house.y);
    visitCells(segmentGrid, segmentCell, house.x, house.y, radius, [&](uint32_t index) {
        const Segment& s = segments[index];
        const float metres = segmentDistance(px, py, static_cast<float>(s.ax), static_cast<float>(s.ay), static_cast<float>(s.bx),
                                 static_cast<float>(s.by)) *
            static_cast<float>(metresPerUnit);
        if (metres > LOW_STREET_M) {
            return;
        }
        // One entry per street name, its nearest distance, sorted
        for (int i = 0; i < count; i++) {
            if (out[i].name == s.name) {
                if (metres < out[i].metres) {
                    out[i].metres = metres;
                    smallSort(out, count, [](const Candidate& a, const Candidate& b) { return a.metres < b.metres; });
                }
                return;
            }
        }
        if (count < CANDIDATES) {
            out[count++] = { s.name, metres };
        } else if (metres < out[count - 1].metres) {
            out[count - 1] = { s.name, metres };
        } else {
            return;
        }
        smallSort(out, count, [](const Candidate& a, const Candidate& b) { return a.metres < b.metres; });
    });
}

void AddressMatcher::villagesFor(const House& house, uint32_t* out) const {
    struct Scored {
        float score;
        uint32_t name;
    };
    Scored best[3];
    int count = 0;
    const auto radius = static_cast<int32_t>(PLACE_M / metresPerUnit);
    visitCells(placeGrid, placeCell, house.x, house.y, radius, [&](uint32_t index) {
        const Place& place = places[index];
        const auto dx = static_cast<float>(place.x - house.x);
        const auto dy = static_cast<float>(place.y - house.y);
        const float metres = std::sqrt(dx * dx + dy * dy) * static_cast<float>(metresPerUnit);
        if (metres > PLACE_M) {
            return;
        }
        const float score = metres / placeClassScale(place.placeClass);
        // Distinct names, the best score of each
        for (int i = 0; i < count; i++) {
            if (best[i].name == place.name) {
                best[i].score = std::min(best[i].score, score);
                smallSort(best, count, [](const Scored& a, const Scored& b) { return a.score < b.score; });
                return;
            }
        }
        if (count < 3) {
            best[count++] = { score, place.name };
        } else if (score < best[2].score) {
            best[2] = { score, place.name };
        } else {
            return;
        }
        smallSort(best, count, [](const Scored& a, const Scored& b) { return a.score < b.score; });
    });
    for (int i = 0; i < 3; i++) {
        out[i] = i < count ? best[i].name : NONE;
    }
}

bool AddressMatcher::run() {
    auto byCell = [](const CellItem& a, const CellItem& b) { return a.cell < b.cell || (a.cell == b.cell && a.item < b.item); };
    std::sort(segmentGrid.begin(), segmentGrid.end(), byCell);
    std::sort(houseGrid.begin(), houseGrid.end(), byCell);
    std::sort(placeGrid.begin(), placeGrid.end(), byCell);
    const size_t count = houses.size();
    if (!results.resize(count) || !candidates.resize(count * CANDIDATES) || segments.failed() || segmentGrid.failed() || houses.failed() ||
        houseGrid.failed() || places.failed() || placeGrid.failed()) {
        return false;
    }
    // 1. The nearest street of each house (the vote needs every neighbour's first)
    for (size_t i = 0; i < count; i++) {
        int found = 0;
        Candidate* own = &candidates[i * CANDIDATES];
        nearestStreets(houses[i], own, found);
        for (int k = found; k < CANDIDATES; k++) {
            own[k] = { NONE, INFINITY };
        }
        AddressMatch& match = results[i];
        match.street = found > 0 ? own[0].name : NONE;
        match.streetM = found > 0 ? own[0].metres : INFINITY;
        match.tier = found == 0 ? AddressTier::None : (own[0].metres <= STREET_M ? AddressTier::Street : AddressTier::LowStreet);
        match.villages[0] = match.villages[1] = match.villages[2] = NONE;
    }
    // 2. The street-run vote, where a second street is nearly as near
    const auto voteUnits = static_cast<int32_t>(VOTE_M / metresPerUnit) + 1;
    const float voteSquared = static_cast<float>(VOTE_M / metresPerUnit) * static_cast<float>(VOTE_M / metresPerUnit);
    for (size_t i = 0; i < count; i++) {
        const House& house = houses[i];
        const Candidate* own = &candidates[i * CANDIDATES];
        if (house.number < 0 || own[0].name == NONE || own[1].name == NONE || own[1].metres > own[0].metres + VOTE_SLACK_M) {
            continue;
        }
        int votes[CANDIDATES] = {};
        visitCells(houseGrid, houseCell, house.x, house.y, voteUnits, [&](uint32_t j) {
            if (j == i) {
                return;
            }
            const House& other = houses[j];
            if (other.number < 0 || (other.number & 1) != (house.number & 1) || std::abs(other.number - house.number) > 4) {
                return;
            }
            const auto dx = static_cast<float>(other.x - house.x);
            const auto dy = static_cast<float>(other.y - house.y);
            if (dx * dx + dy * dy > voteSquared) {
                return;
            }
            // A neighbour votes for its own nearest street
            const uint32_t street = candidates[j * CANDIDATES].name;
            for (int k = 0; k < CANDIDATES; k++) {
                if (own[k].name == street) {
                    votes[k]++;
                }
            }
        });
        int best = 0;
        for (int k = 1; k < CANDIDATES; k++) {
            if (own[k].name != NONE && own[k].metres <= own[0].metres + VOTE_SLACK_M && own[k].metres <= LOW_STREET_M && votes[k] > votes[best]) {
                best = k;
            }
        }
        if (best != 0) {
            AddressMatch& match = results[i];
            match.street = own[best].name;
            match.streetM = own[best].metres;
            match.tier = own[best].metres <= STREET_M ? AddressTier::Street : AddressTier::LowStreet;
        }
    }
    // 3. Settlements for village-style numbers
    for (size_t i = 0; i < count; i++) {
        if (houses[i].villageStyle) {
            villagesFor(houses[i], results[i].villages);
        }
    }
    return true;
}

} // namespace maps
