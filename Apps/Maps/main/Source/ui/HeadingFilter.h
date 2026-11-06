// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

/**
 * The direction the map turns to in heading-up mode, from the GPS course. There is no magnetometer, so the course is
 * only trusted while moving: below MIN_SPEED the last heading is held. Changes are smoothed so GPS jitter does not
 * shake the map.
 */
class HeadingFilter {
public:
    static constexpr float MIN_SPEED = 1.0f;        // m/s
    static constexpr float TIME_CONSTANT_MS = 1500.0f;
    void reset();                                    // no heading
    /** Returns true when heading() changed by at least 1 degree since the last time it returned true. */
    bool update(float courseDegrees, float speedMps, uint32_t nowMs);
    bool hasHeading() const;
    /** Degrees clockwise from north, 0 <= heading < 360. 0 when there is none. */
    float heading() const;
    /** The signed turn from one direction to another, -180 < result <= 180 (shortest way round). */
    static float shortestTurn(float fromDegrees, float toDegrees);

private:
    float value = 0.0f;
    float reported = 0.0f;
    uint32_t lastMs = 0;
    bool hasValue = false;
};

} // namespace maps
