// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The PC stand-in for Tactility's location module: tests set what it reports.

#include "../main/Source/platform/Position.h"

#include <cmath>

namespace maps {

class FakePosition final : public PositionSource {
public:
    bool enabled = true;
    bool hasFix = false;
    PositionFix fix { NAN, NAN, NAN, NAN, NAN, 0 };
    int reads = 0;

    void set(double latitude, double longitude, float accuracy, float course = NAN, float speed = NAN) {
        hasFix = true;
        fix = { latitude, longitude, accuracy, course, speed, 1761700000000 };
    }

    PositionRead read(PositionFix& out) override {
        reads++;
        if (!enabled) {
            return PositionRead::Off;
        }
        if (!hasFix) {
            return PositionRead::NoFix;
        }
        out = fix;
        return PositionRead::Fix;
    }
};

} // namespace maps
