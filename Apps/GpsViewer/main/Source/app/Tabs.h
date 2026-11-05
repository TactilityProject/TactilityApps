// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "AppContext.h"

#include <lvgl.h>

#include <cstdint>

namespace gpsviewer {

struct Theme {
    bool night;
    // Monochrome theme: no colours, shapes and fills only
    bool mono;
    // Screen background behind the app. Only painted in night mode, otherwise the window's own.
    lv_color_t background;
    bool paintBackground;
    // Divider lines
    lv_color_t border;
    lv_color_t accent;
    lv_color_t success;
    lv_color_t warning;
    lv_color_t text;
    // Secondary labels and units
    lv_color_t dim;
    // Radar rings, chart lines
    lv_color_t grid;
};

Theme makeTheme(bool night);
// Day: >= 35 dB-Hz success, 25..34 warning, below that the dim colour. Night: three reds.
lv_color_t snrColor(int snr, const Theme& theme);
lv_color_t systemColor(GnssSystem system, const Theme& theme);
// Sets the label text only when it differs, so unchanged labels cause no redraw
void setLabelText(lv_obj_t* label, const char* text);

// What every tab gets on each refresh. LVGL thread only.
struct TabView {
    const GnssSnapshot& snapshot;
    const Settings& settings;
    SourceStatus status;
    bool recording;
    const char* receiverName;
    const Theme& theme;
};

// Each tab builds its widgets in create() inside `parent` (the content area, about 312x150 on a
// T-Deck, sizes from lv_obj_get_content_width/height), and update() only changes what differs.
// clear() forgets the widget pointers after LVGL deleted them.

class StatusTab {
public:
    void create(lv_obj_t* parent, const Theme& theme);
    void update(const TabView& view);
    void clear();

private:
    lv_obj_t* fixDot = nullptr;
    lv_obj_t* fixLabel = nullptr;
    lv_obj_t* accuracyLabel = nullptr;
    lv_obj_t* dopValue[3] {};
    lv_obj_t* dopQuality[3] {};
    lv_obj_t* satellitesLabel = nullptr;
    lv_obj_t* ttffLabel = nullptr;
    lv_obj_t* ttffNote = nullptr;
    lv_obj_t* receiverLabel = nullptr;
    int lastFix = -1;
};

class SignalTab {
public:
    void create(lv_obj_t* parent, const Theme& theme);
    void update(const TabView& view);
    void clear();
    // Scrolls the bars by a few satellites. Returns false when already at that end.
    bool scroll(int direction);

private:
    static void onDraw(lv_event_t* event);
    static void onPress(lv_event_t* event);
    int contentWidth() const;
    int maxScroll() const;

    lv_obj_t* summaryLabel = nullptr;
    lv_obj_t* bars = nullptr;
    const Theme* theme = nullptr;
    int count = 0;
    Satellite satellites[SatelliteDb::MAX_SATELLITES] {};
    // X of each bar relative to the first one, group gaps included
    int16_t offsets[SatelliteDb::MAX_SATELLITES] {};
    int totalWidth = 0;
    int scrollPx = 0;
    int dragStartX = 0;
    int dragStartScroll = 0;
    bool dragging = false;
};

class SkyTab {
public:
    void create(lv_obj_t* parent, const Theme& theme);
    void update(const TabView& view);
    void clear();

private:
    static void onDrawRadar(lv_event_t* event);
    static void onDrawLegend(lv_event_t* event);

    lv_obj_t* plot = nullptr;
    lv_obj_t* totalLabel = nullptr;
    lv_obj_t* legend = nullptr;
    const Theme* theme = nullptr;
    int count = 0;
    Satellite satellites[SatelliteDb::MAX_SATELLITES] {};
    int inView[GNSS_SYSTEM_COUNT] {};
    int used[GNSS_SYSTEM_COUNT] {};
};

class PositionTab {
public:
    void create(lv_obj_t* parent, const Theme& theme);
    void update(const TabView& view);
    void clear();

private:
    lv_obj_t* latitudeLabel = nullptr;
    lv_obj_t* longitudeLabel = nullptr;
    lv_obj_t* altitudeLabel = nullptr;
    lv_obj_t* courseLabel = nullptr;
    lv_obj_t* speedLabel = nullptr;
    lv_obj_t* speedUnitLabel = nullptr;
    lv_obj_t* speedStatsLabel = nullptr;
    lv_obj_t* riseLabel = nullptr;
    lv_obj_t* setRow = nullptr;
    lv_obj_t* setLabel = nullptr;
    lv_obj_t* timeLabel = nullptr;
    lv_obj_t* recPill = nullptr;
    // Column width the coordinate fonts were picked for
    int fitWidth = -1;
    int sunYear = 0;
    int sunMonth = 0;
    int sunDay = 0;
    double sunLatitude = 1000;
    double sunLongitude = 1000;
    char riseText[24] = "";
    char setText[24] = "";
};

// Centred card for the states without data: alert icon, title, help line and an optional
// OPEN SETTINGS button. It lives in the content area next to the tab panels.
class MessageCard {
public:
    // onSettings is called on click, onKey gets the keys of the focused button
    void create(lv_obj_t* parent, const Theme& theme, lv_event_cb_t onSettings, lv_event_cb_t onKey, void* userData);
    void clear();
    // Shows the card for a status without data and returns true, or hides it and returns false
    bool update(SourceStatus status, const char* dataDirectory);
    lv_obj_t* focusTarget() const { return button; }
    bool shown() const { return visible; }

private:
    lv_obj_t* card = nullptr;
    lv_obj_t* titleLabel = nullptr;
    lv_obj_t* helpLabel = nullptr;
    lv_obj_t* button = nullptr;
    bool visible = false;
    bool buttonInGroup = false;
};

} // namespace gpsviewer
