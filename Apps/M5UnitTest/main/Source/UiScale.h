#pragma once
#include <lvgl.h>
#include <tactility/lvgl_fonts.h>

// Device screen widths in default (portrait) orientation:
//   tiny   < 200  : small OLEDs, custom breadboard devices
//   medium 200-399: M5Stack Core2, Cardputer (~240w)
//   large  400-539: 800x480 devices (~480w portrait)
//   large  540-719: M5PaperS3 (540w), 1024x600 (600w portrait)
//   xlarge 720+   : Tab5 (720w portrait, 1280w landscape)
// uiW() reflects the actual rendered orientation (landscape flips w/h).

static constexpr lv_coord_t UI_TINY_THRESHOLD   = 200;
static constexpr lv_coord_t UI_MEDIUM_THRESHOLD = 400;
static constexpr lv_coord_t UI_LARGE_THRESHOLD  = 540;

inline lv_coord_t uiW() { return lv_display_get_horizontal_resolution(nullptr); }
inline lv_coord_t uiH() { return lv_display_get_vertical_resolution(nullptr); }
inline lv_coord_t uiShortSide() { lv_coord_t w = uiW(), h = uiH(); return w < h ? w : h; }

inline int uiPad() {
    lv_coord_t w = uiW();
    if (w < UI_TINY_THRESHOLD) return 4;
    if (w < UI_MEDIUM_THRESHOLD) return 8;
    if (w < UI_LARGE_THRESHOLD) return 12;
    return 16;
}
inline int uiRowGap() {
    lv_coord_t w = uiW();
    if (w < UI_TINY_THRESHOLD) return 3;
    if (w < UI_MEDIUM_THRESHOLD) return 6;
    if (w < UI_LARGE_THRESHOLD) return 8;
    return 12;
}
inline int uiCols() { return uiW() >= UI_TINY_THRESHOLD ? 2 : 1; }
inline enum LvglFontSize uiFont() {
    lv_coord_t w = uiW();
    return w < UI_TINY_THRESHOLD ? FONT_SIZE_SMALL : (w < UI_LARGE_THRESHOLD ? FONT_SIZE_DEFAULT : FONT_SIZE_LARGE);
}
