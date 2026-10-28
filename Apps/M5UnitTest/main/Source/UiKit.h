#pragma once

#include <lvgl.h>

// ---------------------------------------------------------------------------
// M5UnitTest UI kit
//
// The global LVGL theme (Material) applies focus-ring margins, outline/shadow,
// large paddings and pill radii to every object/button. These test views were
// written against the stock widget defaults, so every widget is created
// through the helpers below. They reset the theme properties first; a view can
// still override any of them afterwards for its own layout.
//
// The macros at the bottom keep the existing view code readable: calls to
// lv_obj_create()/lv_button_create()/lv_label_create() transparently go through
// the neutral helpers.
// ---------------------------------------------------------------------------

static inline lv_color_t uiBgColor()   { return lv_color_hex(0x101418); }
static inline lv_color_t uiPanelColor(){ return lv_color_hex(0x1A2028); }
static inline lv_color_t uiBtnColor()  { return lv_color_hex(0x2F3237); }
static inline lv_color_t uiTextColor() { return lv_color_white(); }

static inline void uiResetWidget(lv_obj_t* obj) {
    lv_obj_set_style_margin_all(obj, 0, 0);
    lv_obj_set_style_outline_width(obj, 0, 0);
    lv_obj_set_style_shadow_width(obj, 0, 0);
    lv_obj_set_style_transform_width(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
}

static inline lv_obj_t* uiObjCreate(lv_obj_t* parent) {
    lv_obj_t* obj = lv_obj_create(parent);
    uiResetWidget(obj);
    return obj;
}

static inline lv_obj_t* uiButtonCreate(lv_obj_t* parent) {
    lv_obj_t* btn = lv_button_create(parent);
    uiResetWidget(btn);
    lv_obj_set_style_bg_color(btn, uiBtnColor(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_pad_hor(btn, 10, 0);
    lv_obj_set_style_pad_ver(btn, 6, 0);
    lv_obj_set_scrollable(btn, false);
    return btn;
}

static inline lv_obj_t* uiLabelCreate(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_style_margin_all(label, 0, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(label, uiTextColor(), 0);
    return label;
}

// Applied after the helper definitions so the helpers themselves still call
// the real LVGL constructors.
#define lv_obj_create uiObjCreate
#define lv_button_create uiButtonCreate
#define lv_label_create uiLabelCreate
