// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

/** The kind of manoeuvre at a point of a route, from the angle between the way in and the way out. */
enum class Turn : uint8_t {
    Straight,
    SlightLeft,
    Left,
    SharpLeft,
    SlightRight,
    Right,
    SharpRight,
    UTurn,
    Arrive,
    Roundabout
};

} // namespace maps
