// SPDX-License-Identifier: GPL-3.0-or-later
#include "Waypoints.h"

#include "../gpx/GpxWriter.h"
#include "../gpx/Track.h"
#include "../render/PreparedTile.h"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

class WaypointCollector final : public GpxListener {
public:
    WaypointCollector(Waypoint* points, int capacity) : points(points), capacity(capacity) {}

    void point(const GpxPoint& point) override {
        if (point.kind != GpxKind::Waypoint || count >= capacity) {
            return;
        }
        Waypoint& waypoint = points[count++];
        snprintf(waypoint.name, sizeof(waypoint.name), "%s", point.name);
        waypoint.lat = point.lat;
        waypoint.lon = point.lon;
        waypoint.time = point.time;
        waypoint.elevation = point.elevation;
    }

    Waypoint* points;
    int capacity;
    int count = 0;
};

bool appendText(ByteWriter& out, const char* text, int length) {
    if (length < 0) {
        return false;
    }
    out.putBytes(text, static_cast<size_t>(length));
    return !out.failed();
}

} // namespace

bool Waypoints::load(const char* text, size_t size) {
    WaypointCollector collector(points, MAX_WAYPOINTS);
    GpxReader reader(collector);
    reader.feed(text, size);
    const bool ok = reader.finish();
    used = collector.count;
    return ok;
}

bool Waypoints::save(ByteWriter& out, const char* appVersion) const {
    char line[384];
    out.clear();
    if (!appendText(out, line, gpxHeader(line, sizeof(line), appVersion))) {
        return false;
    }
    for (int i = 0; i < used; i++) {
        GpxPoint point {};
        point.kind = GpxKind::Waypoint;
        point.lat = points[i].lat;
        point.lon = points[i].lon;
        point.elevation = points[i].elevation;
        point.time = points[i].time;
        snprintf(point.name, sizeof(point.name), "%s", points[i].name);
        if (!appendText(out, line, gpxWaypoint(line, sizeof(line), point))) {
            return false;
        }
    }
    return appendText(out, line, gpxFooter(line, sizeof(line)));
}

void Waypoints::nextName(char* out, size_t size) const {
    // The first free number, so deleting WP 002 lets the next one be WP 002 again
    for (int number = 1; number <= MAX_WAYPOINTS + 1; number++) {
        snprintf(out, size, "WP %03d", number);
        bool taken = false;
        for (int i = 0; i < used && !taken; i++) {
            taken = strcmp(points[i].name, out) == 0;
        }
        if (!taken) {
            return;
        }
    }
}

int Waypoints::add(double lat, double lon, int64_t time, float elevation, const char* name) {
    if (used >= MAX_WAYPOINTS) {
        return -1;
    }
    Waypoint& waypoint = points[used];
    if (name != nullptr && name[0] != '\0') {
        snprintf(waypoint.name, sizeof(waypoint.name), "%s", name);
    } else {
        nextName(waypoint.name, sizeof(waypoint.name));
    }
    waypoint.lat = lat;
    waypoint.lon = lon;
    waypoint.time = time;
    waypoint.elevation = elevation;
    return used++;
}

void Waypoints::remove(int index) {
    if (index < 0 || index >= used) {
        return;
    }
    for (int i = index + 1; i < used; i++) {
        points[i - 1] = points[i];
    }
    used--;
}

void Waypoints::rename(int index, const char* name) {
    if (index >= 0 && index < used && name[0] != '\0') {
        snprintf(points[index].name, sizeof(points[index].name), "%s", name);
    }
}

int Waypoints::sortedByDistance(double lat, double lon, int* out, int max) const {
    float distances[MAX_WAYPOINTS];
    int count = 0;
    for (int i = 0; i < used && count < max; i++) {
        const auto distance = static_cast<float>(distanceMetres(lat, lon, points[i].lat, points[i].lon));
        int slot = count;
        while (slot > 0 && distances[slot - 1] > distance) {
            distances[slot] = distances[slot - 1];
            out[slot] = out[slot - 1];
            slot--;
        }
        distances[slot] = distance;
        out[slot] = i;
        count++;
    }
    return count;
}

} // namespace maps
