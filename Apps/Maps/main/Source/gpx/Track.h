// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../platform/Position.h"

#include <cstdint>

namespace maps {

/** Ground distance in metres between two points, good to well under 1 % for the distances a walk or ride has. */
double distanceMetres(double lat1, double lon1, double lat2, double lon2);

/** Initial bearing from the first point to the second, degrees clockwise from north, 0 to 360. */
double bearingDegrees(double lat1, double lon1, double lat2, double lon2);

/**
 * Decides which position fixes a recorded track keeps: a fix is stored when it moved more than max(5 m, accuracy)
 * from the last stored one, or 30 s after it while still moving. Approximate fixes (IP location) are never stored.
 * Also adds up the distance of the stored points.
 */
class TrackFilter {
public:
    static constexpr double MIN_STEP_METRES = 5.0;
    static constexpr int64_t MOVING_INTERVAL_MS = 30000;
    /** Fixes this inaccurate are approximate positions, like PositionTracker's ring. */
    static constexpr float APPROXIMATE_METRES = 1500.0f;

    /** Whether to store this fix; when it returns true the fix became the last stored point. */
    bool accept(const PositionFix& fix);
    void reset();
    bool hasPoint() const { return stored; }
    double distance() const { return totalMetres; }

private:
    bool stored = false;
    PositionFix last {};
    double totalMetres = 0.0;
};

} // namespace maps
