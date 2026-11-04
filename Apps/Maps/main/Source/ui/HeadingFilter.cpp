// SPDX-License-Identifier: GPL-3.0-or-later
#include "HeadingFilter.h"

#include <algorithm>
#include <cmath>

namespace maps {

namespace {

/** Wraps an angle into [0, 360). */
float normalize(float degrees) {
    float result = fmodf(degrees, 360.0f);
    if (result < 0.0f) {
        result += 360.0f;
    }
    return result;
}

} // namespace

void HeadingFilter::reset() {
    value = 0.0f;
    reported = 0.0f;
    lastMs = 0;
    hasValue = false;
}

bool HeadingFilter::update(float courseDegrees, float speedMps, uint32_t nowMs) {
    if (speedMps < MIN_SPEED || std::isnan(courseDegrees) || courseDegrees < 0.0f) {
        return false;
    }
    const float course = normalize(courseDegrees);
    if (!hasValue) {
        value = course;
        reported = course;
        lastMs = nowMs;
        hasValue = true;
        return true;
    }
    const uint32_t elapsed = nowMs - lastMs;
    const float dt = static_cast<float>(std::min<uint32_t>(elapsed, 5000));
    lastMs = nowMs;
    const float alpha = 1.0f - expf(-dt / TIME_CONSTANT_MS);
    value = normalize(value + alpha * shortestTurn(value, course));
    if (std::fabs(shortestTurn(reported, value)) < 1.0f) {
        return false;
    }
    reported = value;
    return true;
}

bool HeadingFilter::hasHeading() const {
    return hasValue;
}

float HeadingFilter::heading() const {
    return value;
}

float HeadingFilter::shortestTurn(float fromDegrees, float toDegrees) {
    float turn = fmodf(toDegrees - fromDegrees, 360.0f);
    if (turn > 180.0f) {
        turn -= 360.0f;
    } else if (turn <= -180.0f) {
        turn += 360.0f;
    }
    return turn;
}

} // namespace maps
