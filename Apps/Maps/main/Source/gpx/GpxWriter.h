// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GpxReader.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** Formats UTC seconds as 2026-10-30T08:15:00Z into out (at least 21 bytes). */
void formatGpxTime(int64_t seconds, char* out, size_t size);
/** XML-escapes text (& < > " ') into out, cut to fit, always NUL-terminated. */
void escapeXml(const char* text, char* out, size_t size);
/** The opening of a GPX 1.1 file up to and including <gpx ...> with creator="TactilityMaps <version>". */
int gpxHeader(char* out, size_t size, const char* version);
/** "</gpx>\n" */
int gpxFooter(char* out, size_t size);
/** One <wpt lat lon><ele/><time/><name/></wpt> line; elevation NaN or time -1 leave those elements out. */
int gpxWaypoint(char* out, size_t size, const GpxPoint& point);
/** "<trk><name>..</name><trkseg>\n", one "<trkpt ...>...</trkpt>\n", and "</trkseg></trk>\n". */
int gpxTrackStart(char* out, size_t size, const char* name);
int gpxTrackPoint(char* out, size_t size, const GpxPoint& point);
int gpxTrackEnd(char* out, size_t size);

} // namespace maps
