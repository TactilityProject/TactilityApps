// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tabs.h"
#include "Widgets.h"

#include <cmath>
#include <cstdio>

namespace gpsviewer {

namespace {

constexpr float PI_F = 3.14159265f;
constexpr float MARKER_RADIUS = 5.0f;
// Smallest width of the legend sidebar
constexpr int MIN_LEGEND_WIDTH = 96;

bool sameSky(const Satellite& a, const Satellite& b) {
    return a.system == b.system && a.elevation == b.elevation && a.azimuth == b.azimuth && a.used == b.used;
}

} // namespace

void SkyTab::create(lv_obj_t* parent, const Theme& themeIn) {
    theme = &themeIn;
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(parent, LV_DPX(4), LV_PART_MAIN);

    // The radar is as large as the height allows, but leaves room for the legend
    int side = lv_obj_get_content_height(parent);
    const int maxSide = lv_obj_get_content_width(parent) - LV_DPX(4) - LV_DPX(MIN_LEGEND_WIDTH);
    if (side > maxSide) {
        side = maxSide;
    }
    plot = createBox(parent);
    lv_obj_set_size(plot, side, side);
    lv_obj_add_event_cb(plot, onDrawRadar, LV_EVENT_DRAW_MAIN, this);

    lv_obj_t* sidebar = createSection(parent);
    setDivider(sidebar, themeIn, LV_BORDER_SIDE_LEFT);
    lv_obj_set_height(sidebar, LV_PCT(100));
    lv_obj_set_flex_grow(sidebar, 1);
    lv_obj_set_style_pad_all(sidebar, LV_DPX(4), LV_PART_MAIN);
    lv_obj_set_flex_flow(sidebar, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sidebar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_left(sidebar, LV_DPX(8), LV_PART_MAIN);
    lv_obj_set_style_pad_row(sidebar, LV_DPX(4), LV_PART_MAIN);

    createCaption(sidebar, themeIn, "Used / in view");
    totalLabel = createLabel(sidebar, FONT_SIZE_LARGE, themeIn.text);
    legend = createBox(sidebar);
    lv_obj_set_width(legend, LV_PCT(100));
    lv_obj_set_flex_grow(legend, 1);
    lv_obj_add_event_cb(legend, onDrawLegend, LV_EVENT_DRAW_MAIN, this);
    count = 0;
    for (int i = 0; i < GNSS_SYSTEM_COUNT; i++) {
        inView[i] = 0;
        used[i] = 0;
    }
}

void SkyTab::update(const TabView& view) {
    if (plot == nullptr || legend == nullptr) {
        return;
    }
    const GnssSnapshot& s = view.snapshot;
    int n = s.satelliteCount;
    if (n > SatelliteDb::MAX_SATELLITES) {
        n = SatelliteDb::MAX_SATELLITES;
    }
    if (n < 0) {
        n = 0;
    }
    bool changed = n != count;
    for (int i = 0; !changed && i < n; i++) {
        changed = !sameSky(s.satellites[i], satellites[i]);
    }
    if (changed) {
        count = n;
        for (int i = 0; i < n; i++) {
            satellites[i] = s.satellites[i];
        }
        lv_obj_invalidate(plot);
    }

    int newInView[GNSS_SYSTEM_COUNT] = {};
    int newUsed[GNSS_SYSTEM_COUNT] = {};
    for (int i = 0; i < n; i++) {
        const int system = static_cast<int>(s.satellites[i].system);
        if (system < 0 || system >= GNSS_SYSTEM_COUNT) {
            continue;
        }
        newInView[system]++;
        if (s.satellites[i].used) {
            newUsed[system]++;
        }
    }
    bool legendChanged = false;
    for (int i = 0; i < GNSS_SYSTEM_COUNT; i++) {
        if (newInView[i] != inView[i] || newUsed[i] != used[i]) {
            legendChanged = true;
            inView[i] = newInView[i];
            used[i] = newUsed[i];
        }
    }
    if (legendChanged) {
        lv_obj_invalidate(legend);
    }

    char text[32];
    snprintf(text, sizeof(text), "%d / %d", s.satellitesUsed, s.satellitesInView);
    setLabelText(totalLabel, text);
}

void SkyTab::clear() {
    plot = nullptr;
    totalLabel = nullptr;
    legend = nullptr;
    theme = nullptr;
}

void SkyTab::onDrawRadar(lv_event_t* event) {
    auto* self = static_cast<SkyTab*>(lv_event_get_user_data(event));
    if (self->plot == nullptr || self->theme == nullptr) {
        return;
    }
    const Theme& theme = *self->theme;
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(self->plot, &area);
    const int side = area.x2 - area.x1 + 1;
    const int cx = area.x1 + side / 2;
    const int cy = area.y1 + side / 2;
    const int r = side / 2 - 1;
    if (r < 20) {
        return;
    }

    // Rings for 0 (outer), 30 and 60 degrees elevation
    for (int elevation = 0; elevation < 90; elevation += 30) {
        const int rr = r * (90 - elevation) / 90;
        drawCircle(layer, cx, cy, rr, elevation == 0 ? theme.dim : theme.grid, false, 1);
    }
    drawLine(layer, cx, cy - r, cx, cy + r, theme.grid);
    drawLine(layer, cx - r, cy, cx + r, cy, theme.grid);

    const lv_font_t* font = lvgl_get_text_font(FONT_SIZE_SMALL);
    const int lineHeight = lv_font_get_line_height(font);
    const int half = lineHeight / 2 + 1;
    const struct {
        char text[2];
        int x;
        int y;
    } marks[4] = {
        {"N", cx, cy - r + lineHeight / 2 + 2},
        {"E", cx + r - half - 2, cy},
        {"S", cx, cy + r - lineHeight / 2 - 2},
        {"W", cx - r + half + 2, cy},
    };
    for (int i = 0; i < 4; i++) {
        lv_area_t box;
        box.x1 = marks[i].x - half;
        box.x2 = marks[i].x + half;
        box.y1 = marks[i].y - lineHeight / 2;
        box.y2 = box.y1 + lineHeight;
        drawText(layer, marks[i].text, font, theme.dim, box, LV_TEXT_ALIGN_CENTER);
    }

    // Unused first so the used ones end up on top
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < self->count; i++) {
            const Satellite& sat = self->satellites[i];
            if (sat.elevation < 0 || sat.azimuth < 0 || sat.used != (pass == 1)) {
                continue;
            }
            const float distance = static_cast<float>(r) * static_cast<float>(90 - sat.elevation) / 90.0f;
            const float azimuth = static_cast<float>(sat.azimuth) * PI_F / 180.0f;
            const float x = static_cast<float>(cx) + distance * std::sin(azimuth);
            const float y = static_cast<float>(cy) - distance * std::cos(azimuth);
            drawMarker(layer, sat.system, x, y, MARKER_RADIUS, sat.used, systemColor(sat.system, theme));
        }
    }
}

void SkyTab::onDrawLegend(lv_event_t* event) {
    auto* self = static_cast<SkyTab*>(lv_event_get_user_data(event));
    if (self->legend == nullptr || self->theme == nullptr) {
        return;
    }
    const Theme& theme = *self->theme;
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(self->legend, &area);
    const lv_font_t* font = lvgl_get_text_font(FONT_SIZE_SMALL);
    const int lineHeight = lv_font_get_line_height(font);

    int rows = 0;
    for (int i = 0; i < GNSS_SYSTEM_COUNT; i++) {
        if (self->inView[i] > 0) {
            rows++;
        }
    }
    if (rows == 0) {
        lv_area_t box = area;
        box.y1 = area.y1 + ((area.y2 - area.y1 + 1) - lineHeight) / 2;
        box.y2 = box.y1 + lineHeight;
        drawText(layer, "No satellites", font, theme.dim, box, LV_TEXT_ALIGN_CENTER);
        return;
    }
    int pitch = lineHeight + 3;
    const int available = area.y2 - area.y1 + 1;
    if (pitch * rows > available) {
        pitch = available / rows;
    }
    int y = area.y1 + (available - pitch * rows) / 2;
    for (int k = 0; k < GNSS_SYSTEM_COUNT; k++) {
        const int index = (k + 1) % GNSS_SYSTEM_COUNT;
        if (self->inView[index] == 0) {
            continue;
        }
        const auto system = static_cast<GnssSystem>(index);
        const lv_color_t color = systemColor(system, theme);
        const float markerX = static_cast<float>(area.x1) + MARKER_RADIUS + 2.0f;
        const float markerY = static_cast<float>(y) + static_cast<float>(pitch) / 2.0f;
        drawMarker(layer, system, markerX, markerY, MARKER_RADIUS, self->used[index] > 0, color);
        char text[24];
        snprintf(text, sizeof(text), "%s %d/%d", systemName(system), self->used[index], self->inView[index]);
        lv_area_t box;
        box.x1 = area.x1 + static_cast<int>(MARKER_RADIUS * 2.0f) + 8;
        box.x2 = area.x2;
        box.y1 = y + (pitch - lineHeight) / 2;
        box.y2 = box.y1 + lineHeight;
        drawText(layer, text, font, theme.text, box, LV_TEXT_ALIGN_LEFT);
        y += pitch;
    }
}

} // namespace gpsviewer
