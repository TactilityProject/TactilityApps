// SPDX-License-Identifier: GPL-3.0-or-later
#include "RouteTrack.h"

#include "RouteProfile.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

void TrackRouteBuilder::beginLine(GpxKind kind, const char* lineName) {
    (void)kind;
    if (closed) {
        return;
    }
    inLine = true;
    // The name may come with the first point or at the end: the last one given counts
    if (lineName != nullptr && lineName[0] != '\0') {
        snprintf(name, sizeof(name), "%s", lineName);
    }
}

void TrackRouteBuilder::point(const GpxPoint& gpxPoint) {
    if (closed || gpxPoint.kind == GpxKind::Waypoint || !std::isfinite(gpxPoint.lat) || !std::isfinite(gpxPoint.lon)) {
        return;
    }
    read++;
    const RoutePoint here = routePointFrom(gpxPoint.lat, gpxPoint.lon);
    if (points.size() > 0) {
        const RoutePoint& last = points[points.size() - 1];
        const float dx = static_cast<float>(here.x - last.x);
        const float dy = static_cast<float>(here.y - last.y);
        const float metres = std::sqrt(dx * dx + dy * dy) * static_cast<float>(routeMetresPerUnit(here.y));
        if (metres < spacingM) {
            return;
        }
    }
    points.push(here);
    if (points.size() > MAX_POINTS) {
        // Thinned in place: every second point goes, and from now on points come twice as far apart
        size_t kept = 0;
        for (size_t i = 0; i < points.size(); i += 2) {
            points[kept++] = points[i];
        }
        points.resize(kept);
        spacingM *= 2.0f;
    }
}

void TrackRouteBuilder::endLine() {
    if (closed || !inLine) {
        return;
    }
    inLine = false;
    if (points.size() >= 2) {
        closed = true;
    } else {
        // An empty line (or one point): the next line gets its chance
        points.clear();
        name[0] = '\0';
    }
}

bool TrackRouteBuilder::finish(Route& out) {
    out.clear();
    if (points.size() < 2 || points.failed()) {
        return false;
    }
    out.profile = RouteProfile::Walk;
    out.track = true;
    out.from = points[0];
    out.to = points[points.size() - 1];
    float distance = 0.0f;
    for (size_t i = 0; i < points.size(); i++) {
        if (i > 0) {
            const RoutePoint& a = points[i - 1];
            const RoutePoint& b = points[i];
            const float dx = static_cast<float>(b.x - a.x);
            const float dy = static_cast<float>(b.y - a.y);
            distance += std::sqrt(dx * dx + dy * dy) * static_cast<float>(routeMetresPerUnit((a.y + b.y) / 2));
        }
        out.points.push(points[i]);
        out.distances.push(distance);
    }
    out.lengthM = distance;
    out.seconds = distance / (WALK_KMH / 3.6f);
    // One step over the whole line, with its name: the bar and the saved file have something to show
    const size_t length = strlen(name);
    for (size_t i = 0; i < length; i++) {
        out.names.push(name[i]);
    }
    out.steps.push({ 0, 0, static_cast<uint8_t>(length), 0, 0 });
    if (out.points.failed() || out.distances.failed() || out.names.failed() || out.steps.failed()) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace maps
