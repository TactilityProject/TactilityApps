// SPDX-License-Identifier: GPL-3.0-or-later
#include "ElevationProfile.h"

namespace maps {

void ElevationProfile::finish() {
    ascentM = 0.0f;
    descentM = 0.0f;
    if (count <= 0) {
        lowM = highM = 0.0f;
        return;
    }
    lowM = highM = heightM[0];
    // The height last counted from: a change is summed once it passed the step, then counted from there
    float reference = heightM[0];
    for (int i = 1; i < count; i++) {
        const float h = heightM[i];
        lowM = h < lowM ? h : lowM;
        highM = h > highM ? h : highM;
        if (h >= reference + CLIMB_STEP_M) {
            ascentM += h - reference;
            reference = h;
        } else if (h <= reference - CLIMB_STEP_M) {
            descentM += reference - h;
            reference = h;
        }
    }
}

int elevationSampleCount(float lengthM) {
    const int wanted = static_cast<int>(lengthM / 25.0f) + 2;
    return wanted < 2 ? 2 : (wanted > ElevationProfile::MAX_SAMPLES ? ElevationProfile::MAX_SAMPLES : wanted);
}

} // namespace maps
