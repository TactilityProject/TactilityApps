// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

/**
 * Pointer acceleration for a trackball in mouse mode. Tactility's driver only multiplies the ball's steps by the
 * user's pointer sensitivity (10 px per step by default), so slow rolling jumps 10 px at a time and a fast flick is no
 * faster. This scales each movement by a gain that grows smoothly with speed: slow movement gives 1-2 px steps, a
 * fast flick crosses the screen. Fractions of a pixel are kept for the next movement, so slow rolling still moves.
 */
class PointerAcceleration {
public:
    /** Pointer speed setting: 0 slow, 1 normal, 2 fast. It scales the whole curve. */
    static constexpr int SPEEDS = 3;
    /** Gain for slow movement, the fine steps: one 10 px step of the driver becomes 1.5 px. */
    static constexpr float MIN_GAIN = 0.15f;
    /** Gain for a fast flick: 3.5 times Tactility's own pointer speed (device test: 1.6 was still slow). */
    static constexpr float MAX_GAIN = 3.5f;
    /** Speeds in driver pixels per millisecond where the gain starts to grow and where it reaches MAX_GAIN. */
    static constexpr float SLOW_SPEED = 0.1f;
    static constexpr float FAST_SPEED = 0.6f;

    void setSpeed(int setting);
    /** The gain for a speed in driver pixels per millisecond, without the speed setting's factor. */
    static float gainFor(float speed);
    /** Turns the driver's movement since the last call into whole screen pixels. */
    void apply(int32_t dx, int32_t dy, uint32_t now, int& outX, int& outY);
    void reset();

private:
    float factor = 1.0f;
    float speed = 0.0f;
    float restX = 0.0f;
    float restY = 0.0f;
    uint32_t lastMs = 0;
    bool hasLast = false;
};

} // namespace maps
