// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Tabs.h"

#include <lvgl.h>
#include <lvgl/fonts.h>

namespace gpsviewer {

// Widgets: all use local styles only, the platform has no shared style descriptors

// Transparent container without border, padding, scrolling or input
lv_obj_t* createBox(lv_obj_t* parent);
// Flat group of widgets with a little padding, no background or border
lv_obj_t* createSection(lv_obj_t* parent);
// 1 px line in the divider colour on the given sides of the object
void setDivider(lv_obj_t* object, const Theme& theme, lv_border_side_t side);
lv_obj_t* createLabel(lv_obj_t* parent, LvglFontSize size, lv_color_t color);
// Small dim label above a value
lv_obj_t* createCaption(lv_obj_t* parent, const Theme& theme, const char* text);
// Small coloured label for a state, e.g. REC
lv_obj_t* createTag(lv_obj_t* parent, const char* text, lv_color_t color);
void setHidden(lv_obj_t* object, bool hidden);
// Sets the text in the largest of the LARGE, DEFAULT and SMALL fonts that fits maxWidth, only
// when the text differs
void setFittedText(lv_obj_t* label, const char* text, int maxWidth);
// Width of one line of text in the font
int measureTextWidth(const char* text, const lv_font_t* font);

enum class IconKind : uint8_t {
    Sun,
    Moon,
    Alert,
};
// Square icon drawn with the draw calls. Its text colour is the icon colour, its background
// colour (not painted) is the colour of the surface behind it, used to cut out the moon.
lv_obj_t* createIcon(lv_obj_t* parent, IconKind kind, int size, lv_color_t color, lv_color_t surface);

// Drawing, for LV_EVENT_DRAW_MAIN handlers
void drawText(lv_layer_t* layer, const char* text, const lv_font_t* font, lv_color_t color, const lv_area_t& area,
              lv_text_align_t align);
void drawLine(lv_layer_t* layer, int x1, int y1, int x2, int y2, lv_color_t color, int width = 1);
void drawCircle(lv_layer_t* layer, int cx, int cy, int radius, lv_color_t color, bool filled, int borderWidth = 1);
void drawFilledRect(lv_layer_t* layer, int x1, int y1, int x2, int y2, lv_color_t color, int radius = 0);
void fillPolygon(lv_layer_t* layer, const float* xs, const float* ys, int count, lv_color_t color);
// Marker shapes: circle GPS, triangle GLONASS, pentagon Galileo, star BeiDou, square QZSS and SBAS,
// diamond NavIC and unknown. The shape is about 2 * radius wide.
void drawMarker(lv_layer_t* layer, GnssSystem system, float cx, float cy, float radius, bool filled, lv_color_t color);

lv_color_t fixColor(FixType fix, const Theme& theme);
// "3D FIX", "2D FIX" or "NO FIX"
const char* fixText(FixType fix);
// Short constellation name for tight spaces: "GPS", "GLO", "GAL", "BDS", "QZS", "SBS", "NIC", "UNK"
const char* systemShortName(GnssSystem system);

} // namespace gpsviewer
