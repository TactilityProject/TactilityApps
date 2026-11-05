// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tabs.h"
#include "Widgets.h"
#include "../core/SunTimes.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace gpsviewer {

namespace {

void localClock(long long epoch, int& hour, int& minute) {
    time_t t = static_cast<time_t>(epoch);
    struct tm local;
    localtime_r(&t, &local);
    hour = local.tm_hour;
    minute = local.tm_min;
}

const char* compass(float degrees) {
    static const char names[8][3] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    float d = degrees;
    while (d < 0) {
        d += 360.0f;
    }
    const int index = static_cast<int>((d + 22.5f) / 45.0f) % 8;
    return names[index];
}

lv_obj_t* createRow(lv_obj_t* parent, int gap) {
    lv_obj_t* row = createBox(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, gap, LV_PART_MAIN);
    return row;
}

lv_obj_t* createColumn(lv_obj_t* parent) {
    lv_obj_t* card = createSection(parent);
    lv_obj_set_height(card, LV_PCT(100));
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(card, LV_DPX(2), LV_PART_MAIN);
    return card;
}

lv_obj_t* createSpacer(lv_obj_t* parent) {
    lv_obj_t* spacer = createBox(parent);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    return spacer;
}

} // namespace

void PositionTab::create(lv_obj_t* parent, const Theme& theme) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(parent, LV_DPX(2), LV_PART_MAIN);

    lv_obj_t* grid = createBox(parent);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(grid, LV_DPX(6), LV_PART_MAIN);

    // Left tile: coordinates, altitude, course
    lv_obj_t* left = createColumn(grid);
    lv_obj_set_width(left, LV_PCT(56));
    setDivider(left, theme, LV_BORDER_SIDE_RIGHT);
    createCaption(left, theme, "Coordinates");
    latitudeLabel = createLabel(left, FONT_SIZE_LARGE, theme.text);
    longitudeLabel = createLabel(left, FONT_SIZE_LARGE, theme.text);
    createSpacer(left);
    altitudeLabel = createLabel(left, FONT_SIZE_DEFAULT, theme.text);
    courseLabel = createLabel(left, FONT_SIZE_SMALL, theme.dim);

    // Right tile: speed, statistics, sunrise and sunset
    lv_obj_t* right = createColumn(grid);
    lv_obj_set_flex_grow(right, 1);
    createCaption(right, theme, "Speed");
    lv_obj_t* speedRow = createRow(right, LV_DPX(4));
    lv_obj_set_flex_align(speedRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    speedLabel = createLabel(speedRow, FONT_SIZE_LARGE, theme.accent);
    speedUnitLabel = createLabel(speedRow, FONT_SIZE_SMALL, theme.dim);
    speedStatsLabel = createLabel(right, FONT_SIZE_SMALL, theme.dim);
    createSpacer(right);
    const int icon = LV_DPX(12);
    lv_obj_t* sunRow = createRow(right, LV_DPX(3));
    createIcon(sunRow, IconKind::Sun, icon, theme.warning, theme.background);
    riseLabel = createLabel(sunRow, FONT_SIZE_SMALL, theme.text);
    setRow = createRow(right, LV_DPX(3));
    createIcon(setRow, IconKind::Moon, icon, theme.dim, theme.background);
    setLabel = createLabel(setRow, FONT_SIZE_SMALL, theme.text);

    // Bottom status bar: UTC and local time, REC
    lv_obj_t* bar = createRow(parent, LV_DPX(6));
    setDivider(bar, theme, LV_BORDER_SIDE_TOP);
    lv_obj_set_style_pad_top(bar, LV_DPX(3), LV_PART_MAIN);
    lv_obj_set_style_pad_hor(bar, LV_DPX(4), LV_PART_MAIN);
    timeLabel = createLabel(bar, FONT_SIZE_SMALL, theme.dim);
    lv_obj_set_flex_grow(timeLabel, 1);
    recPill = createTag(bar, "REC", theme.warning);
    lv_obj_add_flag(recPill, LV_OBJ_FLAG_HIDDEN);

    fitWidth = -1;
    sunYear = 0;
    sunMonth = 0;
    sunDay = 0;
    sunLatitude = 1000;
    sunLongitude = 1000;
    riseText[0] = '\0';
    setText[0] = '\0';
}

void PositionTab::update(const TabView& view) {
    if (latitudeLabel == nullptr || timeLabel == nullptr) {
        return;
    }
    const GnssSnapshot& s = view.snapshot;
    const Units units = view.settings.units;
    char text[96];
    char part[40];
    const int columnWidth = lv_obj_get_content_width(lv_obj_get_parent(latitudeLabel));
    // Right after a rebuild the layout may not have run yet. Pick the font again once the width is known.
    if (columnWidth != fitWidth) {
        fitWidth = columnWidth;
        lv_label_set_text(latitudeLabel, "");
        lv_label_set_text(longitudeLabel, "");
    }

    if (s.hasPosition) {
        formatCoordinate(s.latitude, true, view.settings.coordinateFormat, part, sizeof(part));
        setFittedText(latitudeLabel, part, columnWidth);
        formatCoordinate(s.longitude, false, view.settings.coordinateFormat, part, sizeof(part));
        setFittedText(longitudeLabel, part, columnWidth);
    } else {
        setFittedText(latitudeLabel, "Lat --", columnWidth);
        setFittedText(longitudeLabel, "Lon --", columnWidth);
    }

    if (s.hasAltitude) {
        formatAltitude(s.altitudeMeters, units, part, sizeof(part));
        snprintf(text, sizeof(text), "Alt %s MSL", part);
    } else {
        snprintf(text, sizeof(text), "Alt --");
    }
    setLabelText(altitudeLabel, text);
    if (!std::isnan(s.courseDegrees)) {
        snprintf(text, sizeof(text), "Course %d\xC2\xB0 %s", static_cast<int>(s.courseDegrees + 0.5f) % 360,
                 compass(s.courseDegrees));
        setLabelText(courseLabel, text);
        setHidden(courseLabel, false);
    } else {
        setHidden(courseLabel, true);
    }

    if (s.hasSpeed) {
        formatSpeed(s.speedMps, units, part, sizeof(part));
    } else {
        snprintf(part, sizeof(part), "--");
    }
    setLabelText(speedLabel, part);
    setLabelText(speedUnitLabel, speedUnit(units));
    char maxText[16];
    char avgText[16];
    formatSpeed(s.maxSpeedMps, units, maxText, sizeof(maxText));
    formatSpeed(s.averageSpeedMps, units, avgText, sizeof(avgText));
    snprintf(text, sizeof(text), "Max %s | Avg %s", maxText, avgText);
    setLabelText(speedStatsLabel, text);

    setHidden(recPill, !view.recording);

    if (s.utc.valid) {
        const long long epoch = utcToEpoch(s.utc.year, s.utc.month, s.utc.day, s.utc.hour, s.utc.minute, s.utc.second);
        int hour;
        int minute;
        localClock(epoch, hour, minute);
        int length = snprintf(text, sizeof(text), "%04d-%02d-%02d %02d:%02d UTC", s.utc.year, s.utc.month, s.utc.day,
                              s.utc.hour, s.utc.minute);
        if ((hour != s.utc.hour || minute != s.utc.minute) && length > 0 && length < static_cast<int>(sizeof(text))) {
            snprintf(text + length, sizeof(text) - static_cast<size_t>(length), "  %02d:%02d local", hour, minute);
        }
        setLabelText(timeLabel, text);
    } else {
        setLabelText(timeLabel, "Time --");
    }

    if (!s.hasPosition || !s.utc.valid) {
        setLabelText(riseLabel, "Sun --");
        setHidden(setRow, true);
        riseText[0] = '\0';
        sunYear = 0;
        return;
    }
    const bool sameDate = s.utc.year == sunYear && s.utc.month == sunMonth && s.utc.day == sunDay;
    const bool samePlace = std::fabs(s.latitude - sunLatitude) <= 0.01 && std::fabs(s.longitude - sunLongitude) <= 0.01;
    if (!sameDate || !samePlace || riseText[0] == '\0') {
        sunYear = s.utc.year;
        sunMonth = s.utc.month;
        sunDay = s.utc.day;
        sunLatitude = s.latitude;
        sunLongitude = s.longitude;
        const SunTimes sun = computeSunTimes(sunYear, sunMonth, sunDay, sunLatitude, sunLongitude);
        if (sun.day == SunDay::PolarDay) {
            snprintf(riseText, sizeof(riseText), "Polar day");
            setText[0] = '\0';
        } else if (sun.day == SunDay::PolarNight) {
            snprintf(riseText, sizeof(riseText), "Polar night");
            setText[0] = '\0';
        } else {
            const long long base = utcToEpoch(sunYear, sunMonth, sunDay, 0, 0, 0);
            int riseHour;
            int riseMinute;
            int setHour;
            int setMinute;
            localClock(base + static_cast<long long>(std::floor(sun.sunriseUtcMinutes * 60.0 + 0.5)), riseHour, riseMinute);
            localClock(base + static_cast<long long>(std::floor(sun.sunsetUtcMinutes * 60.0 + 0.5)), setHour, setMinute);
            snprintf(riseText, sizeof(riseText), "Rise %02d:%02d", riseHour, riseMinute);
            snprintf(setText, sizeof(setText), "Set %02d:%02d", setHour, setMinute);
        }
    }
    setLabelText(riseLabel, riseText);
    setLabelText(setLabel, setText);
    setHidden(setRow, setText[0] == '\0');
}

void PositionTab::clear() {
    latitudeLabel = nullptr;
    longitudeLabel = nullptr;
    altitudeLabel = nullptr;
    courseLabel = nullptr;
    speedLabel = nullptr;
    speedUnitLabel = nullptr;
    speedStatsLabel = nullptr;
    riseLabel = nullptr;
    setRow = nullptr;
    setLabel = nullptr;
    timeLabel = nullptr;
    recPill = nullptr;
}

} // namespace gpsviewer
