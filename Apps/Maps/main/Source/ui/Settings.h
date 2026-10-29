// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace maps {

/**
 * The app's own settings, kept in <root>/settings.txt as "key=value" lines. Choices that only the device test
 * can decide (speed) are settings, so both ways stay available until measured.
 */
struct Settings {
    /**
     * Keep rendered tiles on the SD card as well as in PSRAM. Off by default: on the T-Deck reading a cached
     * tile took about as long as drawing it from its prepared tile, writing one 0.3-0.9 s, and the cache grew to
     * 41 MB. When off, idle time deletes it.
     */
    bool sdCache = false;
    /** Write prepared tiles deflated instead of raw. Both kinds are always read. */
    bool deflatePrepared = true;
    /** Scale the map live while pinching. Never on monochrome displays. */
    bool smoothPinch = true;
    /** Fetch the tiles on screen that have no detail on the card, while Wi-Fi is connected. */
    bool browseDownload = true;
    /** Developer options, only in settings.txt: the monochrome palette on any display, and stall probes. */
    bool devMonochrome = false;
    bool devProbe = false;
};

/** Reads settings from text. Unknown keys and bad lines are ignored, missing keys keep their defaults. */
void parseSettings(const char* text, Settings& settings);
bool formatSettings(const Settings& settings, char* out, size_t size);

bool readSettings(const char* path, Settings& settings);
bool writeSettings(const char* path, const Settings& settings);

/**
 * Where the map was when the app last closed, kept in <root>/view.txt as "lat lon zoom" with the coordinates in
 * millionths of a degree, so no float formatting is needed.
 */
struct SavedView {
    double lat = 0.0;
    double lon = 0.0;
    int zoom = 0;
};

/** False for text that is not a saved view or holds an impossible position. */
bool parseSavedView(const char* text, SavedView& view);
bool formatSavedView(const SavedView& view, char* out, size_t size);

bool readSavedView(const char* path, SavedView& view);
bool writeSavedView(const char* path, const SavedView& view);

} // namespace maps
