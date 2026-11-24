// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Settings.h"
#include "Widgets.h"

#include <cstdint>

namespace maps {

/** The rows of the Settings screen, in their order on it. */
enum class SettingItem : uint8_t {
    SdCache,
    PreparedTiles,
    SmoothPinch,
    BrowseDownload,
    PointerSpeed,
    Labels,
    LatinNames,
    RouteMode,
    WalkTrails,
    Terrain,
    Count
};

/**
 * Fills the Settings list: each row with its value and a line saying what it does and when to change it. monochrome:
 * a display with one map style and no live pinch (Smooth zoom shows as off). The caller sets the hint and lays it out.
 */
void fillSettingsList(ListView& list, const Settings& settings, bool monochrome);

} // namespace maps
