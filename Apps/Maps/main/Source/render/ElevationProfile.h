// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

/**
 * The terrain heights along a route at evenly spaced distances (from the regions' terrain files), with the climb and
 * descent. Changes smaller than CLIMB_STEP_M are not summed, so the 1-2 m noise of the data does not add up.
 */
struct ElevationProfile {
    static constexpr int MAX_SAMPLES = 160;
    static constexpr float CLIMB_STEP_M = 5.0f;

    int count = 0;
    float distanceM[MAX_SAMPLES] {};
    float heightM[MAX_SAMPLES] {};
    float ascentM = 0.0f;
    float descentM = 0.0f;
    float lowM = 0.0f;
    float highM = 0.0f;

    void clear() { count = 0; ascentM = descentM = lowM = highM = 0.0f; }
    /** Sums climb and descent and finds the lowest and highest point of the samples. */
    void finish();
};

/** How many samples a route of this length gets: about one each 25 m, at least 2, at most MAX_SAMPLES. */
int elevationSampleCount(float lengthM);

} // namespace maps
