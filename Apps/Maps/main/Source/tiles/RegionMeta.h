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

    bool isComplete() const { return total > 0 && done >= total; }
    DownloadPlan plan() const { return { box, minZoom, maxZoom, total, 0, true }; }
};

bool writeRegionMeta(const char* path, const RegionMeta& meta);
bool readRegionMeta(const char* path, RegionMeta& meta);
bool formatRegionMeta(const RegionMeta& meta, char* out, size_t size);
bool parseRegionMeta(const char* text, RegionMeta& meta);

} // namespace maps
