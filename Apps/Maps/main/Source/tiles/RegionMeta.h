// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "DownloadPlan.h"
#include "TilePaths.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** The state of a downloaded region, kept in maps/<region>/meta.json so a download resumes after a restart. */
struct RegionMeta {
    char name[REGION_NAME_MAX + 1];
    BoundingBox box;
    int minZoom;
    int maxZoom;
    char source[160];
    char version[48];
    char date[24];
    int64_t total;
    int64_t done;
    uint64_t bytes;
    /** The part of bytes that was moved over from the browsed region, not downloaded: outside the download limit. */
    uint64_t moved;
    /** The catalog region whose outline the tiles follow, empty for a rectangle (Download this area). */
    char outline[64];
    /** The name shown to the user, e.g. "Szeged"; the folder name stays the short 8.3 name. Empty: the folder name. */
    char title[64];

    bool isComplete() const { return total > 0 && done >= total; }
    DownloadPlan plan() const { return { box, minZoom, maxZoom, total, 0, true }; }
};

bool writeRegionMeta(const char* path, const RegionMeta& meta);
/** Sets the display title, made safe for meta.json and cut at a UTF-8 character boundary. */
void setRegionTitle(RegionMeta& meta, const char* title);
/** The title, or the folder name when there is none. */
const char* regionTitle(const RegionMeta& meta);
/**
 * A folder name for a region titled title: its ASCII letters and digits in lower case, at most REGION_NAME_MAX
 * characters, so every path element stays a valid 8.3 name; number > 0 replaces the end with that number, for a
 * name that is taken. "Zuid-Holland" gives "zuidholl", with number 2 "zuidhol2"; a title without letters gives "area".
 */
void regionFolderName(const char* title, int number, char* out, size_t size);
/** Whether two titles are the same, ASCII letters in any case ("Slovakia", "slovakia"). */
bool sameRegionTitle(const char* a, const char* b);
/** A row of the downloaded maps list: one region, or members > 1 regions with the same title. */
struct RegionRow {
    /** The region (the group's first). */
    int region;
    int members;
};
/**
 * The rows of the downloaded maps list for count regions, at most count. With group null or empty, regions with the
 * same title (sameRegionTitle; never the browsed map, never one without a title) are one row where the first of them
 * is; with group set, the regions titled group, one row each. The folders stay apart: each region keeps its own box
 * for updates and the download limits.
 */
int regionRows(const RegionMeta* regions, int count, const char* group, RegionRow* rows);
bool readRegionMeta(const char* path, RegionMeta& meta);
bool formatRegionMeta(const RegionMeta& meta, char* out, size_t size);
bool parseRegionMeta(const char* text, RegionMeta& meta);

} // namespace maps
