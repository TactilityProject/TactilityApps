// SPDX-License-Identifier: GPL-3.0-or-later
#include "CatalogBrowser.h"

#include "../search/TextFold.h"

#include <cstdio>
#include <cstring>

namespace maps {

void CatalogBrowser::showLevel(const RegionCatalog& catalog, int level) {
    parent = level;
    results = false;
    rows.clear();
    char name[ListView::TEXT_LENGTH];
    int count = 0;
    if (level < 0) {
        // The whole catalog by name: faster than walking continents for a small country
        rows.setTitle("Choose region");
        rows.add("Search by name", "/");
        regions[count++] = SEARCH_ROW;
    } else {
        catalog.copyName(level, name, sizeof(name));
        rows.setTitle(name);
        char label[ListView::TEXT_LENGTH];
        snprintf(label, sizeof(label), "All of %s", name);
        rows.add(label);
        regions[count++] = level;
    }
    int children[ListView::MAX_ITEMS];
    const int found = catalog.children(level, children, ListView::MAX_ITEMS - count);
    for (int i = 0; i < found; i++) {
        catalog.copyName(children[i], name, sizeof(name));
        rows.add(name, catalog.region(children[i]).childCount > 0 ? ">" : "");
        regions[count++] = children[i];
    }
    rows.select(0);
}

bool CatalogBrowser::showResults(const RegionCatalog& catalog, const char* query) {
    char wanted[ListView::TEXT_LENGTH];
    if (foldLatin(query, wanted, sizeof(wanted)) == 0) {
        return false;
    }
    char name[ListView::TEXT_LENGTH];
    char folded[ListView::TEXT_LENGTH];
    int found[ListView::MAX_ITEMS];
    int count = 0;
    for (int i = 0; i < catalog.count() && count < ListView::MAX_ITEMS; i++) {
        catalog.copyName(i, name, sizeof(name));
        foldLatin(name, folded, sizeof(folded));
        if (strstr(folded, wanted) != nullptr) {
            found[count++] = i;
        }
    }
    if (count == 0) {
        return false;
    }
    parent = -1;
    results = true;
    rows.clear();
    char title[ListView::TEXT_LENGTH];
    snprintf(title, sizeof(title), "Regions: %s", query);
    rows.setTitle(title);
    for (int i = 0; i < count; i++) {
        catalog.copyName(found[i], name, sizeof(name));
        // The larger region it belongs to, so two of a name can be told apart
        const int above = catalog.region(found[i]).parent;
        char detail[ListView::TEXT_LENGTH] = "";
        if (above >= 0) {
            catalog.copyName(above, detail, sizeof(detail));
        }
        if (catalog.region(found[i]).childCount > 0) {
            const size_t used = strlen(detail);
            snprintf(detail + used, sizeof(detail) - used, "%s>", used > 0 ? " " : "");
        }
        rows.add(name, detail);
        regions[i] = found[i];
    }
    rows.select(0);
    return true;
}

CatalogBrowser::Choice CatalogBrowser::choose(const RegionCatalog& catalog, int& region) {
    const int row = rows.selected();
    if (row < 0 || row >= rows.count()) {
        return Choice::None;
    }
    region = regions[row];
    if (region == SEARCH_ROW) {
        return Choice::Search;
    }
    // A region with parts opens its list; "All of ..." and regions without parts are downloaded
    if (region != parent && catalog.region(region).childCount > 0) {
        showLevel(catalog, region);
        return Choice::Opened;
    }
    return Choice::Download;
}

bool CatalogBrowser::back(const RegionCatalog& catalog) {
    if (results) {
        // From the results back to the top of the catalog
        showLevel(catalog, -1);
        return true;
    }
    if (parent < 0) {
        return false;
    }
    const int child = parent;
    showLevel(catalog, catalog.region(parent).parent);
    // Back on the row it came from
    for (int row = 0; row < rows.count(); row++) {
        if (regions[row] == child) {
            rows.select(row);
        }
    }
    return true;
}

} // namespace maps
