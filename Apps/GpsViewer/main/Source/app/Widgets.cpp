// SPDX-License-Identifier: GPL-3.0-or-later
#include "Widgets.h"

#include <cmath>
#include <cstring>

namespace gpsviewer {

namespace {

constexpr int MAX_POINTS = 10;
constexpr float PI_F = 3.14159265f;

// lv_obj_get_style_*_color are not exported to apps, so the icon keeps its colours in the user data
uintptr_t packColor(lv_color_t color) {
    return (static_cast<uintptr_t>(color.red) << 16) | (static_cast<uintptr_t>(color.green) << 8) | color.blue;
}

lv_color_t unpackColor(uintptr_t packed) {
    lv_color_t color;
    color.red = static_cast<uint8_t>(packed >> 16);
    color.green = static_cast<uint8_t>(packed >> 8);
    color.blue = static_cast<uint8_t>(packed);
    return color;
}

// Width of one line of UTF-8 text, as lv_text_get_size() is not exported to apps
int textWidth(const char* text, const lv_font_t* font) {
    int width = 0;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    while (*bytes != 0) {
        uint32_t letter = *bytes++;
        int extra = 0;
        if (letter >= 0xF0) {
            letter &= 0x07;
            extra = 3;
        } else if (letter >= 0xE0) {
            letter &= 0x0F;
            extra = 2;
        } else if (letter >= 0xC0) {
            letter &= 0x1F;
            extra = 1;
        }
        for (; extra > 0 && (*bytes & 0xC0) == 0x80; extra--) {
            letter = (letter << 6) | (*bytes++ & 0x3F);
        }
        width += lv_font_get_glyph_width(font, letter, *bytes);
    }
    return width;
}

int roundInt(float value) {
    return static_cast<int>(std::floor(value + 0.5f));
}

void drawIcon(lv_event_t* event) {
    lv_obj_t* object = lv_event_get_current_target_obj(event);
    lv_layer_t* layer = lv_event_get_layer(event);
    const auto packed = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(object));
    const auto kind = static_cast<IconKind>(packed >> 24);
    const lv_color_t color = unpackColor(packed & 0xFFFFFFU);
    lv_area_t area;
    lv_obj_get_coords(object, &area);
    const int size = area.x2 - area.x1 + 1;
    const float cx = static_cast<float>(area.x1) + static_cast<float>(size - 1) / 2.0f;
    const float cy = static_cast<float>(area.y1) + static_cast<float>(size - 1) / 2.0f;
    const float half = static_cast<float>(size) / 2.0f;
    switch (kind) {
        case IconKind::Sun: {
            drawCircle(layer, roundInt(cx), roundInt(cy), roundInt(half * 0.4f + 0.5f), color, true);
            for (int i = 0; i < 8; i++) {
                const float angle = static_cast<float>(i) * PI_F / 4.0f;
                const float c = std::cos(angle);
                const float s = std::sin(angle);
                drawLine(layer, roundInt(cx + c * half * 0.65f), roundInt(cy + s * half * 0.65f),
                         roundInt(cx + c * (half - 0.5f)), roundInt(cy + s * (half - 0.5f)), color);
            }
            break;
        }
        case IconKind::Moon: {
            const lv_color_t surface = unpackColor(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
            drawCircle(layer, roundInt(cx), roundInt(cy), roundInt(half * 0.85f), color, true);
            drawCircle(layer, roundInt(cx + half * 0.5f), roundInt(cy - half * 0.3f), roundInt(half * 0.7f), surface, true);
            break;
        }
        case IconKind::Alert: {
            const float xs[3] = {cx, cx + half - 0.5f, cx - half + 0.5f};
            const float ys[3] = {cy - half + 1.0f, cy + half - 1.5f, cy + half - 1.5f};
            for (int i = 0; i < 3; i++) {
                const int j = (i + 1) % 3;
                drawLine(layer, roundInt(xs[i]), roundInt(ys[i]), roundInt(xs[j]), roundInt(ys[j]), color, 2);
            }
            // Exclamation mark: bar and dot
            const int x = roundInt(cx);
            const int top = roundInt(cy - half * 0.35f);
            const int bottom = roundInt(cy + half * 0.2f);
            drawFilledRect(layer, x - 1, top, x, bottom, color);
            drawFilledRect(layer, x - 1, bottom + 3, x, bottom + 4, color);
            break;
        }
    }
}

} // namespace

lv_obj_t* createBox(lv_obj_t* parent) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_outline_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(box, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_column(box, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(box, 0, LV_PART_MAIN);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_flag(box, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(box, LV_OBJ_FLAG_GESTURE_BUBBLE);
    if (lv_group_get_default() != nullptr) {
        lv_group_remove_obj(box);
    }
    return box;
}

lv_obj_t* createSection(lv_obj_t* parent) {
    lv_obj_t* section = createBox(parent);
    lv_obj_set_style_pad_all(section, LV_DPX(4), LV_PART_MAIN);
    return section;
}

void setDivider(lv_obj_t* object, const Theme& theme, lv_border_side_t side) {
    lv_obj_set_style_border_color(object, theme.border, LV_PART_MAIN);
    lv_obj_set_style_border_width(object, 1, LV_PART_MAIN);
    lv_obj_set_style_border_side(object, side, LV_PART_MAIN);
}

lv_obj_t* createLabel(lv_obj_t* parent, LvglFontSize size, lv_color_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(size), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_label_set_text(label, "");
    return label;
}

lv_obj_t* createCaption(lv_obj_t* parent, const Theme& theme, const char* text) {
    lv_obj_t* label = createLabel(parent, FONT_SIZE_SMALL, theme.dim);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t* createTag(lv_obj_t* parent, const char* text, lv_color_t color) {
    lv_obj_t* tag = createLabel(parent, FONT_SIZE_SMALL, color);
    lv_label_set_text(tag, text);
    return tag;
}

void setHidden(lv_obj_t* object, bool hidden) {
    if (object == nullptr) {
        return;
    }
    if (hidden == lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)) {
        return;
    }
    if (hidden) {
        lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
    }
}

void setFittedText(lv_obj_t* label, const char* text, int maxWidth) {
    if (label == nullptr || strcmp(lv_label_get_text(label), text) == 0) {
        return;
    }
    const LvglFontSize sizes[3] = {FONT_SIZE_LARGE, FONT_SIZE_DEFAULT, FONT_SIZE_SMALL};
    const lv_font_t* font = lvgl_get_text_font(FONT_SIZE_SMALL);
    for (int i = 0; i < 3; i++) {
        font = lvgl_get_text_font(sizes[i]);
        if (textWidth(text, font) <= maxWidth) {
            break;
        }
    }
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    lv_label_set_text(label, text);
}

lv_obj_t* createIcon(lv_obj_t* parent, IconKind kind, int size, lv_color_t color, lv_color_t surface) {
    lv_obj_t* icon = createBox(parent);
    lv_obj_set_size(icon, size, size);
    const uintptr_t packed = (static_cast<uintptr_t>(kind) << 24) | packColor(color);
    lv_obj_set_user_data(icon, reinterpret_cast<void*>(packed));
    lv_obj_add_event_cb(icon, drawIcon, LV_EVENT_DRAW_MAIN, reinterpret_cast<void*>(packColor(surface)));
    return icon;
}

void drawText(lv_layer_t* layer, const char* text, const lv_font_t* font, lv_color_t color, const lv_area_t& area,
              lv_text_align_t align) {
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font = font;
    dsc.color = color;
    dsc.text = text;
    dsc.text_local = 1;
    dsc.align = align;
    lv_draw_label(layer, &dsc, &area);
}

void drawLine(lv_layer_t* layer, int x1, int y1, int x2, int y2, lv_color_t color, int width) {
    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = color;
    dsc.width = width;
    dsc.p1.x = x1;
    dsc.p1.y = y1;
    dsc.p2.x = x2;
    dsc.p2.y = y2;
    lv_draw_line(layer, &dsc);
}

void drawCircle(lv_layer_t* layer, int cx, int cy, int radius, lv_color_t color, bool filled, int borderWidth) {
    lv_area_t box;
    box.x1 = cx - radius;
    box.y1 = cy - radius;
    box.x2 = cx + radius;
    box.y2 = cy + radius;
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.radius = LV_RADIUS_CIRCLE;
    if (filled) {
        rect.bg_opa = LV_OPA_COVER;
        rect.bg_color = color;
        rect.border_width = 0;
    } else {
        rect.bg_opa = LV_OPA_TRANSP;
        rect.border_opa = LV_OPA_COVER;
        rect.border_color = color;
        rect.border_width = borderWidth;
        rect.border_side = LV_BORDER_SIDE_FULL;
    }
    lv_draw_rect(layer, &rect, &box);
}

void drawFilledRect(lv_layer_t* layer, int x1, int y1, int x2, int y2, lv_color_t color, int radius) {
    lv_area_t box;
    box.x1 = x1;
    box.y1 = y1;
    box.x2 = x2;
    box.y2 = y2;
    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.radius = radius;
    rect.bg_opa = LV_OPA_COVER;
    rect.bg_color = color;
    rect.border_width = 0;
    lv_draw_rect(layer, &rect, &box);
}

// Even-odd scanline fill, works for convex and concave outlines
void fillPolygon(lv_layer_t* layer, const float* px, const float* py, int n, lv_color_t color) {
    float top = py[0];
    float bottom = py[0];
    for (int i = 1; i < n; i++) {
        if (py[i] < top) {
            top = py[i];
        }
        if (py[i] > bottom) {
            bottom = py[i];
        }
    }
    lv_draw_fill_dsc_t fill;
    lv_draw_fill_dsc_init(&fill);
    fill.color = color;
    fill.radius = 0;
    const int yStart = static_cast<int>(std::floor(top));
    const int yEnd = static_cast<int>(std::ceil(bottom));
    for (int y = yStart; y <= yEnd; y++) {
        const float yc = static_cast<float>(y) + 0.5f;
        float xs[MAX_POINTS];
        int m = 0;
        for (int i = 0; i < n; i++) {
            const int j = (i + 1) % n;
            const float lo = py[i] < py[j] ? py[i] : py[j];
            const float hi = py[i] < py[j] ? py[j] : py[i];
            if (hi > lo && yc >= lo && yc < hi && m < MAX_POINTS) {
                xs[m++] = px[i] + (yc - py[i]) * (px[j] - px[i]) / (py[j] - py[i]);
            }
        }
        for (int i = 1; i < m; i++) {
            const float value = xs[i];
            int j = i;
            while (j > 0 && xs[j - 1] > value) {
                xs[j] = xs[j - 1];
                j--;
            }
            xs[j] = value;
        }
        for (int i = 0; i + 1 < m; i += 2) {
            lv_area_t span;
            span.x1 = roundInt(xs[i]);
            span.x2 = roundInt(xs[i + 1]) - 1;
            if (span.x2 < span.x1) {
                span.x2 = span.x1;
            }
            span.y1 = y;
            span.y2 = y;
            lv_draw_fill(layer, &fill, &span);
        }
    }
}

void drawMarker(lv_layer_t* layer, GnssSystem system, float cx, float cy, float radius, bool filled, lv_color_t color) {
    const float r = radius;
    float xs[MAX_POINTS];
    float ys[MAX_POINTS];
    int n = 0;
    switch (system) {
        case GnssSystem::Gps:
            drawCircle(layer, roundInt(cx), roundInt(cy), roundInt(r), color, filled, 2);
            return;
        case GnssSystem::Glonass:
            xs[n] = cx;
            ys[n++] = cy - r * 1.15f;
            xs[n] = cx + r * 1.1f;
            ys[n++] = cy + r * 0.85f;
            xs[n] = cx - r * 1.1f;
            ys[n++] = cy + r * 0.85f;
            break;
        case GnssSystem::Galileo:
            for (int i = 0; i < 5; i++) {
                const float angle = (-90.0f + 72.0f * static_cast<float>(i)) * PI_F / 180.0f;
                xs[n] = cx + r * 1.05f * std::cos(angle);
                ys[n++] = cy + r * 1.05f * std::sin(angle);
            }
            break;
        case GnssSystem::Beidou:
            for (int i = 0; i < 10; i++) {
                const float angle = (-90.0f + 36.0f * static_cast<float>(i)) * PI_F / 180.0f;
                const float radiusI = (i % 2 == 0) ? r * 1.2f : r * 0.5f;
                xs[n] = cx + radiusI * std::cos(angle);
                ys[n++] = cy + radiusI * std::sin(angle);
            }
            break;
        case GnssSystem::Qzss:
        case GnssSystem::Sbas: {
            const float h = r * 0.85f;
            xs[n] = cx - h;
            ys[n++] = cy - h;
            xs[n] = cx + h;
            ys[n++] = cy - h;
            xs[n] = cx + h;
            ys[n++] = cy + h;
            xs[n] = cx - h;
            ys[n++] = cy + h;
            break;
        }
        default:
            xs[n] = cx;
            ys[n++] = cy - r * 1.2f;
            xs[n] = cx + r * 1.2f;
            ys[n++] = cy;
            xs[n] = cx;
            ys[n++] = cy + r * 1.2f;
            xs[n] = cx - r * 1.2f;
            ys[n++] = cy;
            break;
    }
    if (filled) {
        fillPolygon(layer, xs, ys, n, color);
        return;
    }
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        drawLine(layer, roundInt(xs[i]), roundInt(ys[i]), roundInt(xs[j]), roundInt(ys[j]), color);
    }
}

lv_color_t fixColor(FixType fix, const Theme& theme) {
    if (fix == FixType::Fix3d) {
        return theme.success;
    }
    return fix == FixType::Fix2d ? theme.warning : theme.dim;
}

const char* fixText(FixType fix) {
    return fix == FixType::Fix3d ? "3D FIX" : (fix == FixType::Fix2d ? "2D FIX" : "NO FIX");
}

const char* systemShortName(GnssSystem system) {
    switch (system) {
        case GnssSystem::Gps: return "GPS";
        case GnssSystem::Glonass: return "GLO";
        case GnssSystem::Galileo: return "GAL";
        case GnssSystem::Beidou: return "BDS";
        case GnssSystem::Qzss: return "QZS";
        case GnssSystem::Sbas: return "SBS";
        case GnssSystem::Navic: return "NIC";
        default: return "UNK";
    }
}

} // namespace gpsviewer
