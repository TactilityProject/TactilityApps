#pragma once

#include <lvgl.h>
#include <lvgl/fonts.h>

// ---------------------------------------------------------------------------
// MidiController UI kit
//
// Fixed-pixel instrument UI. The global LVGL theme (Material) styles every
// button/container with focus-ring margins, large paddings, pill radii and its
// own colours, so all widgets are created through these helpers, which reset
// those theme defaults back to the stock values the layouts are written for.
//
// Layout metrics are deliberately shared: every view uses the same row height,
// padding, gap and control widths so the transport/control rows look identical
// from one mode to the next.
// ---------------------------------------------------------------------------

// --- Palette ---------------------------------------------------------------

static inline lv_color_t MC_BG()       { return lv_color_hex(0x080C12); }  // view background
static inline lv_color_t MC_PANEL()    { return lv_color_hex(0x0A0C14); }  // row / control strip
static inline lv_color_t MC_SURFACE()  { return lv_color_hex(0x1A2A40); }  // raised cells / keys
static inline lv_color_t MC_BTN()      { return lv_color_hex(0x2F3237); }  // neutral button
static inline lv_color_t MC_BTN_ALT()  { return lv_color_hex(0x222233); }  // back / secondary button
static inline lv_color_t MC_LABEL()    { return lv_color_hex(0x666688); }  // captions
static inline lv_color_t MC_VALUE()    { return lv_color_hex(0x00DFFF); }  // readouts
static inline lv_color_t MC_PRIMARY()  { return lv_color_hex(0x00DFFF); }  // accent / highlight
static inline lv_color_t MC_OK()       { return lv_color_hex(0x00AA44); }  // play / start
static inline lv_color_t MC_STOP()     { return lv_color_hex(0xFF2020); }  // stop / recording
static inline lv_color_t MC_TEXT()     { return lv_color_white(); }

// --- Metrics ---------------------------------------------------------------

constexpr int MC_ROW_H    = 88;                              // transport/control row height
constexpr int MC_ROW_PAD  = 12;                              // row inner padding
constexpr int MC_ROW_GAP  = 12;                              // gap between row children
constexpr int MC_BTN_H    = MC_ROW_H - 2 * MC_ROW_PAD;       // 64
constexpr int MC_HDR_H    = 88;                              // header row
constexpr int MC_STATUS_H = 56;                              // bottom status strip

// Control widths (shared tiers - pick the smallest that fits the row).
constexpr int MC_W_XS  = 80;
constexpr int MC_W_S   = 88;
constexpr int MC_W_M   = 112;
constexpr int MC_W_L   = 144;
constexpr int MC_W_XL  = 176;
constexpr int MC_W_XXL = 224;

// Readout-label widths.
constexpr int MC_LBL_S = 96;
constexpr int MC_LBL_M = 144;
constexpr int MC_LBL_L = 176;
constexpr int MC_LBL_XL = 224;

// --- Widget primitives -----------------------------------------------------

static inline lv_obj_t* mcContainer(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_pad_gap(obj, 0, 0);
    return obj;
}

static inline lv_obj_t* mcButton(lv_obj_t* parent) {
    lv_obj_t* btn = lv_button_create(parent);
    // Stock dark-theme button defaults: no focus-ring margin, rounded rect, grey fill.
    lv_obj_set_style_margin_all(btn, 0, 0);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, MC_BTN(), 0);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 8, 0);
    return btn;
}

static inline lv_obj_t* mcLabel(lv_obj_t* parent, const char* txt,
                                const lv_font_t* font, lv_color_t color,
                                int width = 0) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, color, 0);
    if (width > 0) {
        lv_obj_set_width(lbl, width);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    }
    return lbl;
}

// --- Row / control builders ------------------------------------------------

// Panel-styled flex row with the shared height/padding/gap.
static inline lv_obj_t* mcRow(lv_obj_t* parent, int height = MC_ROW_H) {
    lv_obj_t* row = mcContainer(parent);
    lv_obj_set_size(row, LV_PCT(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(row, MC_PANEL(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(row, MC_ROW_PAD, 0);
    lv_obj_set_style_pad_gap(row, MC_ROW_GAP, 0);
    lv_obj_set_scrollable(row, false);
    return row;
}

// Small text button on a neutral fill.
static inline lv_obj_t* mcBtn(lv_obj_t* parent, const char* txt, int w, int h,
                              lv_event_cb_t cb, void* user) {
    lv_obj_t* btn = mcButton(parent);
    lv_obj_set_size(btn, w, h);
    if (cb != nullptr) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    }
    lv_obj_t* lbl = mcLabel(btn, txt, lvgl_get_text_font(FONT_SIZE_SMALL), MC_TEXT());
    lv_obj_center(lbl);
    return btn;
}

static inline lv_obj_t* mcSmallBtn(lv_obj_t* parent, const char* txt, int w,
                                   lv_event_cb_t cb, void* user) {
    return mcBtn(parent, txt, w, MC_BTN_H, cb, user);
}

// Primary transport button ("PLAY"/"START").
static inline lv_obj_t* mcPrimaryBtn(lv_obj_t* parent, const char* txt, int w,
                                     lv_event_cb_t cb, void* user) {
    lv_obj_t* btn = mcSmallBtn(parent, txt, w, cb, user);
    lv_obj_set_style_bg_color(btn, MC_OK(), 0);
    return btn;
}

// Back / secondary navigation button.
static inline lv_obj_t* mcBackBtn(lv_obj_t* parent, lv_event_cb_t cb, void* user,
                                  int w = MC_W_M) {
    lv_obj_t* btn = mcSmallBtn(parent, "<", w, cb, user);
    lv_obj_set_style_bg_color(btn, MC_BTN_ALT(), 0);
    return btn;
}

// Centered readout value.
static inline lv_obj_t* mcValue(lv_obj_t* parent, const char* txt, int w,
                                lv_color_t color = MC_VALUE()) {
    return mcLabel(parent, txt, lvgl_get_text_font(FONT_SIZE_SMALL), color, w);
}

// Small caption ("Rate:", "Key:", ...).
static inline lv_obj_t* mcHead(lv_obj_t* parent, const char* txt) {
    return mcLabel(parent, txt, lvgl_get_text_font(FONT_SIZE_SMALL), MC_LABEL());
}

// Per-mode title: normal font, accent colour, grows to fill the row.
static inline lv_obj_t* mcTitle(lv_obj_t* parent, const char* txt, lv_color_t accent) {
    lv_obj_t* lbl = mcLabel(parent, txt, lvgl_get_text_font(FONT_SIZE_DEFAULT), accent);
    lv_obj_set_flex_grow(lbl, 1);
    return lbl;
}
