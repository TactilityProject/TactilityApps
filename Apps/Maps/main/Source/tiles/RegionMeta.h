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
bool readRegionMeta(const char* path, RegionMeta& meta);
bool formatRegionMeta(const RegionMeta& meta, char* out, size_t size);
bool parseRegionMeta(const char* text, RegionMeta& meta);

} // namespace maps
