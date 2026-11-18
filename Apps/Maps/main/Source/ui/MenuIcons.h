// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Canvas.h"

#include <cstdint>

namespace maps {

/** The pictures of the menu entries. */
enum class MenuIcon : uint8_t {
    None,
    Search,
    Download,
    Maps,
    Waypoints,
    Track,
    Gpx,
    Location,
    Settings,
    About,
    Exit,
    Route,
    Stop,
    Clear,
    Pause
};

/**
 * Draws an icon as plain lines in one colour, centred on a point, in a square of size pixels. They are drawn for 24
 * and still read at 18. Nothing depends on a second colour, so they work on a black and white display.
 */
void drawMenuIcon(Canvas& canvas, MenuIcon icon, int centreX, int centreY, int size, uint16_t color);

} // namespace maps
