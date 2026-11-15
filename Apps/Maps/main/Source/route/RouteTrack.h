// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RouteSearch.h"

#include "../gpx/GpxReader.h"

namespace maps {

/**
 * Makes a route out of a GPX file's first track or route (0.6.0), so the follower can lead along it like along a
 * route the search found: feed the file to a GpxReader with this as its listener, then finish().
 *
 * Only the first line with at least two points is taken (all segments of a track). Points closer than a few metres
 * to the one before are left out, and a very long track is thinned while it is read, so memory stays bounded.
 * The route has no turns (it is not known which roads the track follows): one step with the line's name, the
 * walking time for its length, and `track` set.
 */
class TrackRouteBuilder : public GpxListener {
public:
    /** No route is given more points: a long track is thinned to fit (every second point goes, again and again). */
    static constexpr size_t MAX_POINTS = 6000;

    void beginLine(GpxKind kind, const char* name) override;
    void point(const GpxPoint& point) override;
    void endLine() override;

    /** False when the file had no line of two points or memory ran out; out is then empty. */
    bool finish(Route& out);
    size_t pointsRead() const { return read; }

private:
    GrowArray<RoutePoint> points;
    char name[48] = "";
    /** A line with at least two points was read to its end: later lines are not taken. */
    bool closed = false;
    bool inLine = false;
    /** Points closer than this to the last one kept are left out; grows when the track is thinned. */
    float spacingM = 2.0f;
    size_t read = 0;
};

} // namespace maps
