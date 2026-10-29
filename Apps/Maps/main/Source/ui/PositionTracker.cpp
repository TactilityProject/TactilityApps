// SPDX-License-Identifier: GPL-3.0-or-later
#include "PositionTracker.h"

#include <cmath>

namespace maps {

bool PositionTracker::update(PositionSource& source) {
    PositionFix fix {};
    const PositionRead result = source.read(fix);
    PositionState next;
    if (result == PositionRead::Off) {
        next = PositionState::Off;
    } else if (result == PositionRead::NoFix || std::isnan(fix.latitude) || std::isnan(fix.longitude)) {
        next = PositionState::Waiting;
    } else if (!std::isnan(fix.accuracy) && fix.accuracy >= APPROXIMATE_ACCURACY_METRES) {
        next = PositionState::Approximate;
    } else {
        next = PositionState::Precise;
    }
    const bool hasNewFix = next == PositionState::Approximate || next == PositionState::Precise;
    bool changed = next != currentState;
    if (hasNewFix) {
        const auto same = [](float a, float b) { return a == b || (std::isnan(a) && std::isnan(b)); };
        changed = changed || fix.latitude != lastFix.latitude || fix.longitude != lastFix.longitude ||
            !same(fix.accuracy, lastFix.accuracy) || !same(fix.course, lastFix.course) || !same(fix.speed, lastFix.speed);
        lastFix = fix;
    }
    currentState = next;
    return changed;
}

bool PositionTracker::showsArrow() const {
    return currentState == PositionState::Precise && !std::isnan(lastFix.course) && !std::isnan(lastFix.speed) &&
        lastFix.speed > MOVING_SPEED;
}

float PositionTracker::accuracyRadius(double metresPerPixel) const {
    if (!hasFix() || std::isnan(lastFix.accuracy) || lastFix.accuracy <= 0.0f || metresPerPixel <= 0.0) {
        return 0.0f;
    }
    return static_cast<float>(lastFix.accuracy / metresPerPixel);
}

const char* PositionTracker::unavailableMessage() const {
    if (currentState == PositionState::Off) {
        return "Location is off. Enable it in Tactility's Settings.";
    }
    return "No position yet. Check Location in Tactility's Settings.";
}

} // namespace maps
