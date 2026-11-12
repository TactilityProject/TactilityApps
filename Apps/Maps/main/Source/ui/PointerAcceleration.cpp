// SPDX-License-Identifier: GPL-3.0-or-later
#include "PointerAcceleration.h"

#include <algorithm>
#include <cmath>

namespace maps {

namespace {

constexpr float SPEED_FACTORS[PointerAcceleration::SPEEDS] = { 0.6f, 1.0f, 1.7f };
/** The trackball reports in bursts: time between movements is counted as at least one input poll, at most a pause. */
constexpr uint32_t MIN_INTERVAL_MS = 15;
constexpr uint32_t MAX_INTERVAL_MS = 120;
/** How much a new speed reading counts against the smoothed speed. */
constexpr float SMOOTHING = 0.5f;

} // namespace

void PointerAcceleration::setSpeed(int setting) {
    factor = SPEED_FACTORS[std::clamp(setting, 0, SPEEDS - 1)];
}

float PointerAcceleration::gainFor(float value) {
    const float t = std::clamp((value - SLOW_SPEED) / (FAST_SPEED - SLOW_SPEED), 0.0f, 1.0f);
    // Smoothstep, so the gain has no corner where slow turns into fast
    const float eased = t * t * (3.0f - 2.0f * t);
    return MIN_GAIN + (MAX_GAIN - MIN_GAIN) * eased;
}

void PointerAcceleration::apply(int32_t dx, int32_t dy, uint32_t now, int& outX, int& outY) {
    outX = 0;
    outY = 0;
    if (dx == 0 && dy == 0) {
        // A pause ends the movement: the next one starts slow again
        if (hasLast && now - lastMs > MAX_INTERVAL_MS) {
            speed = 0.0f;
            restX = 0.0f;
            restY = 0.0f;
        }
        return;
    }
    const uint32_t interval = hasLast ? std::clamp(now - lastMs, MIN_INTERVAL_MS, MAX_INTERVAL_MS) : MAX_INTERVAL_MS;
    lastMs = now;
    hasLast = true;
    const float distance = std::sqrt(static_cast<float>(dx) * static_cast<float>(dx) + static_cast<float>(dy) * static_cast<float>(dy));
    speed = speed * (1.0f - SMOOTHING) + (distance / static_cast<float>(interval)) * SMOOTHING;
    const float gain = gainFor(speed) * factor;
    restX += static_cast<float>(dx) * gain;
    restY += static_cast<float>(dy) * gain;
    outX = static_cast<int>(restX);
    outY = static_cast<int>(restY);
    restX -= static_cast<float>(outX);
    restY -= static_cast<float>(outY);
}

void PointerAcceleration::reset() {
    speed = 0.0f;
    restX = 0.0f;
    restY = 0.0f;
    hasLast = false;
}

} // namespace maps
