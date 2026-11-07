// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tabs.h"
#include "Widgets.h"

#include <cstdio>

namespace gpsviewer {

namespace {

constexpr int BAR_WIDTH = 14;
constexpr int BAR_SPACING = 4;
constexpr int BAR_PITCH = BAR_WIDTH + BAR_SPACING;
// Extra space between two constellation groups
constexpr int GROUP_GAP = 8;
// Left strip with the scale labels
constexpr int GUTTER = 20;
constexpr int PAGE_BARS = 4;
constexpr int MAX_SNR = 50;

int systemRank(GnssSystem system) {
    return system == GnssSystem::Unknown ? 99 : static_cast<int>(system);
}

bool before(const Satellite& a, const Satellite& b) {
    if (a.system != b.system) {
        return systemRank(a.system) < systemRank(b.system);
    }
    return a.id < b.id;
}

bool sameDrawn(const Satellite& a, const Satellite& b) {
    return a.system == b.system && a.id == b.id && a.snr == b.snr && a.used == b.used;
}

} // namespace

void SignalTab::create(lv_obj_t* parent, const Theme& themeIn) {
    theme = &themeIn;
    lv_obj_t* card = createSection(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(card, LV_DPX(4), LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, LV_DPX(2), LV_PART_MAIN);

    summaryLabel = createLabel(card, FONT_SIZE_SMALL, themeIn.text);
    lv_obj_set_width(summaryLabel, LV_PCT(100));

    bars = createBox(card);
    lv_obj_set_width(bars, LV_PCT(100));
    lv_obj_set_flex_grow(bars, 1);
    // The bars are dragged, so the tab swipe must not fire on top of that
    lv_obj_remove_flag(bars, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(bars, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(bars, onDraw, LV_EVENT_DRAW_MAIN, this);
    lv_obj_add_event_cb(bars, onPress, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(bars, onPress, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(bars, onPress, LV_EVENT_RELEASED, this);
    dragging = false;
    count = 0;
    totalWidth = 0;
}

int SignalTab::contentWidth() const {
    return lv_obj_get_width(bars) - GUTTER;
}

int SignalTab::maxScroll() const {
    const int overflow = totalWidth - contentWidth();
    return overflow > 0 ? overflow : 0;
}

void SignalTab::update(const TabView& view) {
    if (summaryLabel == nullptr || bars == nullptr) {
        return;
    }
    const GnssSnapshot& s = view.snapshot;

    Satellite sorted[SatelliteDb::MAX_SATELLITES];
    int n = s.satelliteCount;
    if (n > SatelliteDb::MAX_SATELLITES) {
        n = SatelliteDb::MAX_SATELLITES;
    }
    if (n < 0) {
        n = 0;
    }
    for (int i = 0; i < n; i++) {
        const Satellite value = s.satellites[i];
        int j = i;
        while (j > 0 && before(value, sorted[j - 1])) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = value;
    }

    bool changed = n != count;
    for (int i = 0; !changed && i < n; i++) {
        changed = !sameDrawn(sorted[i], satellites[i]);
    }
    if (changed) {
        count = n;
        int x = 0;
        for (int i = 0; i < n; i++) {
            satellites[i] = sorted[i];
            if (i > 0) {
                x += BAR_PITCH + (satellites[i].system != satellites[i - 1].system ? GROUP_GAP : 0);
            }
            offsets[i] = static_cast<int16_t>(x);
        }
        totalWidth = n > 0 ? x + BAR_WIDTH : 0;
        const int limit = maxScroll();
        if (scrollPx > limit) {
            scrollPx = limit;
        }
        lv_obj_invalidate(bars);
    }

    int sum = 0;
    int used = 0;
    for (int i = 0; i < n; i++) {
        if (sorted[i].used && sorted[i].snr > 0) {
            sum += sorted[i].snr;
            used++;
        }
    }
    char text[96];
    if (used > 0) {
        snprintf(text, sizeof(text), "Avg SNR (used) %d dB-Hz   %d used / %d in view", (sum + used / 2) / used,
                 s.satellitesUsed, s.satellitesInView);
    } else {
        snprintf(text, sizeof(text), "Avg SNR (used) --   %d used / %d in view", s.satellitesUsed, s.satellitesInView);
    }
    setLabelText(summaryLabel, text);
}

void SignalTab::clear() {
    summaryLabel = nullptr;
    bars = nullptr;
    theme = nullptr;
    dragging = false;
}

bool SignalTab::scroll(int direction) {
    if (bars == nullptr) {
        return false;
    }
    const int limit = maxScroll();
    if (direction > 0 && scrollPx < limit) {
        scrollPx += PAGE_BARS * BAR_PITCH;
        if (scrollPx > limit) {
            scrollPx = limit;
        }
        lv_obj_invalidate(bars);
        return true;
    }
    if (direction < 0 && scrollPx > 0) {
        scrollPx -= PAGE_BARS * BAR_PITCH;
        if (scrollPx < 0) {
            scrollPx = 0;
        }
        lv_obj_invalidate(bars);
        return true;
    }
    return false;
}

void SignalTab::onPress(lv_event_t* event) {
    auto* self = static_cast<SignalTab*>(lv_event_get_user_data(event));
    if (self->bars == nullptr) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_RELEASED) {
        self->dragging = false;
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (code == LV_EVENT_PRESSED) {
        self->dragging = true;
        self->dragStartX = p.x;
        self->dragStartScroll = self->scrollPx;
        return;
    }
    if (!self->dragging) {
        return;
    }
    int next = self->dragStartScroll - (p.x - self->dragStartX);
    const int limit = self->maxScroll();
    if (next < 0) {
        next = 0;
    }
    if (next > limit) {
        next = limit;
    }
    if (next != self->scrollPx) {
        self->scrollPx = next;
        lv_obj_invalidate(self->bars);
    }
}

void SignalTab::onDraw(lv_event_t* event) {
    auto* self = static_cast<SignalTab*>(lv_event_get_user_data(event));
    if (self->bars == nullptr || self->theme == nullptr) {
        return;
    }
    const Theme& theme = *self->theme;
    lv_layer_t* layer = lv_event_get_layer(event);
    lv_area_t area;
    lv_obj_get_coords(self->bars, &area);
    const lv_font_t* font = lvgl_get_text_font(FONT_SIZE_SMALL);
    const int lineHeight = lv_font_get_line_height(font);

    if (self->count == 0) {
        lv_area_t box = area;
        box.y1 = area.y1 + ((area.y2 - area.y1 + 1) - lineHeight) / 2;
        box.y2 = box.y1 + lineHeight;
        drawText(layer, "No satellites in view", font, theme.dim, box, LV_TEXT_ALIGN_CENTER);
        return;
    }

    const int limit = self->maxScroll();
    if (self->scrollPx > limit) {
        self->scrollPx = limit;
    }
    const int left = area.x1 + GUTTER;
    const int origin = left - self->scrollPx;

    const int markerRow = 9;
    const int chartBottom = area.y2 - lineHeight - markerRow;
    const int chartTop = area.y1 + lineHeight + 3;
    const int chartHeight = chartBottom - chartTop;
    if (chartHeight < 10) {
        return;
    }

    // Scale lines at the colour thresholds, drawn first so the bars cover them
    const int levels[2] = {25, 35};
    for (int i = 0; i < 2; i++) {
        const int y = chartBottom - levels[i] * chartHeight / MAX_SNR;
        drawLine(layer, left, y, area.x2, y, theme.grid);
    }
    drawLine(layer, left, chartBottom, area.x2, chartBottom, theme.dim);

    int groupStart = 0;
    for (int i = 0; i < self->count; i++) {
        const Satellite& sat = self->satellites[i];
        const bool lastOfGroup = i == self->count - 1 || self->satellites[i + 1].system != sat.system;
        const int x = origin + self->offsets[i];
        if (x + BAR_WIDTH >= left && x <= area.x2) {
            const int snr = sat.snr > MAX_SNR ? MAX_SNR : (sat.snr < 0 ? 0 : sat.snr);
            const lv_color_t color = snrColor(snr, theme);
            if (snr == 0) {
                drawLine(layer, x, chartBottom - 1, x + BAR_WIDTH - 1, chartBottom - 1, theme.dim);
            } else {
                int height = snr * chartHeight / MAX_SNR;
                if (height < 3) {
                    height = 3;
                }
                lv_area_t bar;
                bar.x1 = x;
                bar.x2 = x + BAR_WIDTH - 1;
                bar.y1 = chartBottom - height;
                bar.y2 = chartBottom - 1;
                lv_draw_rect_dsc_t rect;
                lv_draw_rect_dsc_init(&rect);
                rect.radius = 2;
                if (sat.used) {
                    rect.bg_opa = LV_OPA_COVER;
                    rect.bg_color = color;
                    rect.border_width = 0;
                } else {
                    rect.bg_opa = LV_OPA_TRANSP;
                    rect.border_opa = LV_OPA_COVER;
                    rect.border_color = color;
                    rect.border_width = 1;
                    rect.border_side = LV_BORDER_SIDE_FULL;
                }
                lv_draw_rect(layer, &rect, &bar);
            }

            const float centre = static_cast<float>(x) + static_cast<float>(BAR_WIDTH - 1) / 2.0f;
            drawMarker(layer, sat.system, centre, static_cast<float>(chartBottom + markerRow / 2 + 1), 2.5f, true,
                       systemColor(sat.system, theme));
            lv_area_t idBox;
            idBox.x1 = x - BAR_SPACING / 2 - 3;
            idBox.x2 = x + BAR_WIDTH + BAR_SPACING / 2 + 2;
            idBox.y1 = area.y2 - lineHeight + 1;
            idBox.y2 = area.y2;
            char id[8];
            snprintf(id, sizeof(id), "%d", static_cast<int>(sat.id));
            drawText(layer, id, font, theme.text, idBox, LV_TEXT_ALIGN_CENTER);
        }

        if (lastOfGroup) {
            // Group label at the start of the group, kept in view while the group is scrolled
            const int groupLeft = origin + self->offsets[groupStart];
            const int groupRight = x + BAR_WIDTH;
            const int labelWidth = 3 * lineHeight / 2 + 4;
            int labelX = groupLeft < left ? left : groupLeft;
            if (labelX + labelWidth > groupRight) {
                labelX = groupRight - labelWidth;
            }
            if (groupRight > left && groupLeft < area.x2) {
                lv_area_t box;
                box.x1 = labelX;
                box.x2 = labelX + labelWidth;
                box.y1 = area.y1;
                box.y2 = area.y1 + lineHeight;
                drawText(layer, systemShortName(sat.system), font, systemColor(sat.system, theme), box,
                         LV_TEXT_ALIGN_LEFT);
            }
            groupStart = i + 1;
        }
    }

    // Scale strip on top of the scrolled content
    drawFilledRect(layer, area.x1, area.y1, left - 3, area.y2, theme.background);
    for (int i = 0; i < 2; i++) {
        const int y = chartBottom - levels[i] * chartHeight / MAX_SNR;
        lv_area_t box;
        box.x1 = area.x1;
        box.x2 = left - 4;
        box.y1 = y - lineHeight / 2;
        box.y2 = box.y1 + lineHeight;
        char label[8];
        snprintf(label, sizeof(label), "%d", levels[i]);
        drawText(layer, label, font, theme.dim, box, LV_TEXT_ALIGN_LEFT);
    }
}

} // namespace gpsviewer
