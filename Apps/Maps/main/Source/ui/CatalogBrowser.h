// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/RegionCatalog.h"
#include "Widgets.h"

namespace maps {

/**
 * The region catalog as a list (Download, Choose region): the continents with "Search by name" on top, a region's
 * parts with "All of ..." first, or the regions whose names hold a text. It fills the list and says what a chosen row
 * means; the map screen lays it out, shows it and downloads.
 */
class CatalogBrowser {
public:
    enum class Choice : uint8_t {
        None,
        /** "Search by name": the caller asks for the text and calls showResults. */
        Search,
        /** A region with parts: its list is shown now. */
        Opened,
        /** A region to download (one without parts, or "All of ..."). */
        Download
    };

    /** The parts of parent (-1: the continents, with the search row), the first row selected. */
    void showLevel(const RegionCatalog& catalog, int parent);
    /** Every region whose name holds query (case and accents ignored); false and nothing changed when none does. */
    bool showResults(const RegionCatalog& catalog, const char* query);
    /** What the selected row means; for Download the region is in region. */
    Choice choose(const RegionCatalog& catalog, int& region);
    /** One level up (results go to the top), on the row it came from; false at the top: the caller leaves. */
    bool back(const RegionCatalog& catalog);

    ListView& list() { return rows; }
    bool showsResults() const { return results; }

private:
    static constexpr int SEARCH_ROW = -2;
    ListView rows;
    /** The region whose parts are shown, -1 for the continents (and for results). */
    int parent = -1;
    bool results = false;
    int regions[ListView::MAX_ITEMS] {};
};

} // namespace maps
