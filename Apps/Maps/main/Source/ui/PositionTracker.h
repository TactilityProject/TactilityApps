// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../platform/Position.h"

#include <cstdint>

namespace maps {

enum class PositionState : uint8_t {
    Off,
    Waiting,
    /** A fix about as coarse as an IP lookup, drawn as a circle without a direction. */
    Approximate,
    Precise
};

/** The current position as the map draws it, read from a PositionSource. */
class PositionTracker {
public:
    /** Fixes at least this coarse count as approximate. Tactility reports 3,000 m for approximate fixes. */
    static constexpr float APPROXIMATE_ACCURACY_METRES = 1500.0f;
    /** The direction arrow shows only above this speed. */
    static constexpr float MOVING_SPEED = 1.0f;

    /** Reads the source. Returns true when what the map draws changed. */
    bool update(PositionSource& source);

    PositionState state() const { return currentState; }
    bool hasFix() const { return currentState == PositionState::Approximate || currentState == PositionState::Precise; }
    const PositionFix& fix() const { return lastFix; }
    bool showsArrow() const;
    /** Radius of the accuracy circle in pixels, 0 when the accuracy is unknown. */
    float accuracyRadius(double metresPerPixel) const;
    /** What a press on the position button says while there is no fix. */
    const char* unavailableMessage() const;

private:
    PositionState currentState = PositionState::Waiting;
    PositionFix lastFix {};
};

} // namespace maps
