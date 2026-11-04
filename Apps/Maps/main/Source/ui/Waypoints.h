// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../gpx/GpxReader.h"

#include <cstddef>
#include <cstdint>

namespace maps {

class ByteWriter;

struct Waypoint {
    char name[48];
    double lat;
    double lon;
    /** Seconds since 1970 UTC, -1 when unknown. */
    int64_t time;
    /** Metres, NaN when unknown. */
    float elevation;
};

/**
 * The user's waypoints, kept in waypoints.gpx in the app's folder (GPX 1.1). The list lives in memory; the app
 * saves it as a whole after each change, through a .tmp file.
 */
class Waypoints {
public:
    static constexpr int MAX_WAYPOINTS = 200;

    /** Reads GPX text, replacing the list. Waypoints beyond MAX_WAYPOINTS are dropped. False when it is not GPX. */
    bool load(const char* text, size_t size);
    /** Writes the list as GPX 1.1 into out. */
    bool save(ByteWriter& out, const char* appVersion) const;

    int count() const { return used; }
    const Waypoint& at(int index) const { return points[index]; }
    /**
     * Adds a waypoint named name, or "WP 001", "WP 002", ... (the first number not in use) when name is null or empty.
     * Returns its index or -1 when full.
     */
    int add(double lat, double lon, int64_t time, float elevation, const char* name = nullptr);
    /** The name the next waypoint gets without one of its own. */
    void nextName(char* out, size_t size) const;
    void remove(int index);
    void rename(int index, const char* name);
    /** Indices sorted by distance from a point, nearest first; returns how many. */
    int sortedByDistance(double lat, double lon, int* out, int max) const;

private:
    Waypoint points[MAX_WAYPOINTS] {};
    int used = 0;
};

} // namespace maps
