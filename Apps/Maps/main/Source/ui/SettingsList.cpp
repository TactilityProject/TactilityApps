// SPDX-License-Identifier: GPL-3.0-or-later
#include "SettingsList.h"

#include "../route/RouteProfile.h"

#include <algorithm>
#include <cstddef>

namespace maps {

void fillSettingsList(ListView& list, const Settings& settings, bool monochrome) {
    list.clear();
    list.setTitle("Settings");
    list.add("SD card cache", settings.sdCache ? "On" : "Off");
    list.add("Prepared tiles", settings.deflatePrepared ? "Deflated" : "Raw");
    list.add("Smooth zoom", monochrome ? "Off (display)" : (settings.smoothPinch ? "On" : "Off"));
    list.add("Download while browsing", settings.browseDownload ? "On" : "Off");
    static const char* const POINTER_SPEEDS[] = { "Slow", "Normal", "Fast" };
    list.add("Pointer speed", POINTER_SPEEDS[std::clamp(settings.pointerSpeed, 0, 2)]);
    static const char* const LABEL_DENSITIES[] = { "Off", "Few", "Normal" };
    list.add("Labels", LABEL_DENSITIES[std::clamp(settings.labels, 0, 2)]);
    list.add("Latin names", settings.latinNames ? "On" : "Off");
    list.add("Route mode", routeProfileName(static_cast<RouteProfile>(std::clamp(settings.routeProfile, 0, 2))));
    list.add("Walk prefers paths", settings.walkTrails ? "On" : "Off");
    list.add("Terrain", settings.terrain ? "On" : "Off");
    // In the order of SettingItem: what each does and when to change it, under its name
    static const char* const DESCRIPTIONS[] = {
        "Keeps drawn tiles: faster revisits, uses card",
        "Deflated saves card space, Raw draws faster",
        "Scales while pinching; Off on slow drawing",
        "Fetches missing areas on Wi-Fi as you pan",
        "Trackball cursor speed in pointer mode",
        "Fewer: a cleaner map that draws faster",
        "On abroad: names you can read",
        "The mode new routes start with",
        "On for hikes, Off for the shortest way",
        "Shading, contours; downloads terrain",
    };
    static_assert(sizeof(DESCRIPTIONS) / sizeof(DESCRIPTIONS[0]) == static_cast<size_t>(SettingItem::Count));
    list.setDescriptions(DESCRIPTIONS, static_cast<int>(SettingItem::Count));
}

} // namespace maps
