#include "TestUnitJoystick2.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>
#include <algorithm>
#include <cmath>

void TestUnitJoystick2::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;

    createToolbar(parent, handle, "Joystick2");
    createBanner(parent, "Joystick2", "I2C", COLOR_I2C);

    // Scale joystick area to shorter display dimension, clamped 80..300px
    lv_coord_t minDim = std::min(uiW(), uiH());
    int JOY_AREA = (int)(minDim * 3 / 10);
    if (JOY_AREA < 80)  JOY_AREA = 80;
    if (JOY_AREA > 300) JOY_AREA = 300;
    int DOT_SIZE = JOY_AREA / 8;
    if (DOT_SIZE < 8)  DOT_SIZE = 8;

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, uiRowGap(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    const lv_font_t* fnt = lvgl_get_text_font(uiFont());

    lblXY_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblXY_, fnt, 0);

    lblButton_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblButton_, fnt, 0);

    joyArea_ = JOY_AREA;
    dotSize_ = DOT_SIZE;

    // Visual joystick area
    joyCont_ = lv_obj_create(cont);
    lv_obj_set_size(joyCont_, JOY_AREA, JOY_AREA);
    lv_obj_set_style_bg_color(joyCont_, lv_color_hex(0x222222), 0);
    lv_obj_set_style_radius(joyCont_, JOY_AREA / 2, 0);
    lv_obj_set_style_border_width(joyCont_, 2, 0);
    lv_obj_set_style_pad_all(joyCont_, 0, 0);
    lv_obj_remove_flag(joyCont_, LV_OBJ_FLAG_SCROLLABLE);

    dot_ = lv_obj_create(joyCont_);
    lv_obj_set_size(dot_, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(dot_, DOT_SIZE / 2, 0);
    lv_obj_set_style_bg_color(dot_, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(dot_, 0, 0);
    lv_obj_set_pos(dot_, (JOY_AREA - DOT_SIZE) / 2, (JOY_AREA - DOT_SIZE) / 2);

    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) {
        lv_label_set_text(lblXY_, "i2c1 not found");
        return;
    }

    if (unit_.begin(i2c)) {
        usingPaHub_ = false;
    } else if (hub_.begin(i2c)) {
        usingPaHub_ = true;
        bool found = false;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !found; ch++) {
            hub_.select(ch);
            if (unit_.begin(i2c)) found = true;
        }
        if (!found) {
            hub_.deselect();
            lv_label_set_text(lblXY_, "Joystick2 not found");
            return;
        }
    } else {
        lv_label_set_text(lblXY_, "Joystick2 not found");
        return;
    }

    timer_ = lv_timer_create(onTimer, 50, this);
    update();
}

void TestUnitJoystick2::onStop() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    selectIfNeeded();
    if (unit_.isPresent()) unit_.setLed(0x000000);
    if (usingPaHub_ && hub_.isPresent()) hub_.deselect();
    lblXY_ = lblButton_ = dot_ = joyCont_ = nullptr;
}

void TestUnitJoystick2::selectIfNeeded() {
    if (usingPaHub_ && hub_.isPresent())
        hub_.select(hub_.currentChannel());
}

void TestUnitJoystick2::onTimer(lv_timer_t* t) {
    static_cast<TestUnitJoystick2*>(lv_timer_get_user_data(t))->update();
}

void TestUnitJoystick2::update() {
    selectIfNeeded();
    if (!unit_.isPresent()) return;

    int16_t x = 0, y = 0;
    unit_.readXY12(&x, &y);
    bool pressed = unit_.isPressed();
    lv_label_set_text_fmt(lblXY_, "X: %d  Y: %d", (int)x, (int)y);
    lv_label_set_text_fmt(lblButton_, "Button: %s", pressed ? "PRESSED" : "released");

    // Map ±2048 joystick range to dot position within a circle.
    // Work in float to do circular clamping, then snap back to int pixels.
    // Negate both axes to match LVGL screen coordinates and joystick orientation.
    // Grove connector facing away from the user.
    float radius = (float)(joyArea_ - dotSize_) / 2.0f;
    float nx = -(float)x / 2048.0f;  // normalised -1..1
    float ny = -(float)y / 2048.0f;  // normalised -1..1
    float dist2 = nx * nx + ny * ny;
    if (dist2 > 1.0f) {
        float inv = 1.0f / std::sqrt(dist2);
        nx *= inv;
        ny *= inv;
    }
    int cx = (int)(radius + nx * radius);
    int cy = (int)(radius + ny * radius);
    lv_obj_set_pos(dot_, cx, cy);

    // LED: hue cycles with X position, blue when pressed
    if (pressed) {
        unit_.setLed(0x0000FF);
    } else {
        uint16_t hue = (uint16_t)((int)x * 360 / 4096 + 180);  // map -2048..2048 -> 0..360
        lv_color_t c = lv_color_hsv_to_rgb(hue % 360, 100, 78);
        lv_color32_t c32 = lv_color_to_32(c, LV_OPA_COVER);
        unit_.setLed(((uint32_t)c32.red << 16) | ((uint32_t)c32.green << 8) | c32.blue);
    }

    lv_obj_set_style_bg_color(dot_, pressed ? lv_color_hex(0xFF4400) : lv_color_hex(0x00FF00), 0);
}
