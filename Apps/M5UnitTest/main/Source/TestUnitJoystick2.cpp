#include "TestUnitJoystick2.h"
#include "M5UnitTest.h"
#include "GroveLookup.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/fonts.h>
#include <algorithm>
#include <cmath>

namespace {

void selectIfNeeded(TestUnitJoystick2* self) {
    if (self->usingPaHub_ && self->hub_.isPresent())
        self->hub_.select(self->hub_.currentChannel());
}

void update(TestUnitJoystick2* self) {
    selectIfNeeded(self);
    if (!self->unit_.isPresent()) return;

    int16_t x = 0, y = 0;
    self->unit_.readXY12(&x, &y);
    bool pressed = self->unit_.isPressed();
    lv_label_set_text_fmt(self->lblXY_, "X: %d  Y: %d", (int)x, (int)y);
    lv_label_set_text_fmt(self->lblButton_, "Button: %s", pressed ? "PRESSED" : "released");

    // Map ±2048 joystick range to dot position within a circle.
    // Work in float to do circular clamping, then snap back to int pixels.
    // Negate both axes to match LVGL screen coordinates and joystick orientation.
    // Grove connector facing away from the user.
    float radius = (float)(self->joyArea_ - self->dotSize_) / 2.0f;
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
    lv_obj_set_pos(self->dot_, cx, cy);

    // LED: hue cycles with X position, blue when pressed
    if (pressed) {
        self->unit_.setLed(0x0000FF);
    } else {
        uint16_t hue = (uint16_t)((int)x * 360 / 4096 + 180);  // map -2048..2048 -> 0..360
        lv_color_t c = lv_color_hsv_to_rgb(hue % 360, 100, 78);
        lv_color32_t c32 = lv_color_to_32(c, LV_OPA_COVER);
        self->unit_.setLed(((uint32_t)c32.red << 16) | ((uint32_t)c32.green << 8) | c32.blue);
    }

    lv_obj_set_style_bg_color(self->dot_, pressed ? lv_color_hex(0xFF4400) : lv_color_hex(0x00FF00), 0);
}

void onTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitJoystick2*>(lv_timer_get_user_data(t));
    if (window_manager_get_state(self->app_->window) != WINDOW_STATE_GRANTED) return;
    update(self);
}

} // namespace

void testUnitJoystick2Start(TestUnitJoystick2* self, lv_obj_t* parent, Context* app) {
    self->app_ = app;

    testViewCreateToolbar(parent, app, "Joystick2");
    testViewCreateBanner(parent, "Joystick2", "I2C", COLOR_I2C);

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

    self->lblXY_ = lv_label_create(cont);
    lv_obj_set_style_text_font(self->lblXY_, fnt, 0);

    self->lblButton_ = lv_label_create(cont);
    lv_obj_set_style_text_font(self->lblButton_, fnt, 0);

    self->joyArea_ = JOY_AREA;
    self->dotSize_ = DOT_SIZE;

    // Visual joystick area
    self->joyCont_ = lv_obj_create(cont);
    lv_obj_set_size(self->joyCont_, JOY_AREA, JOY_AREA);
    lv_obj_set_style_bg_color(self->joyCont_, lv_color_hex(0x222222), 0);
    lv_obj_set_style_radius(self->joyCont_, JOY_AREA / 2, 0);
    lv_obj_set_style_border_width(self->joyCont_, 2, 0);
    lv_obj_set_style_pad_all(self->joyCont_, 0, 0);
    lv_obj_remove_flag(self->joyCont_, LV_OBJ_FLAG_SCROLLABLE);

    self->dot_ = lv_obj_create(self->joyCont_);
    lv_obj_set_size(self->dot_, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(self->dot_, DOT_SIZE / 2, 0);
    lv_obj_set_style_bg_color(self->dot_, lv_color_hex(0x00FF00), 0);
    lv_obj_set_style_border_width(self->dot_, 0, 0);
    lv_obj_set_pos(self->dot_, (JOY_AREA - DOT_SIZE) / 2, (JOY_AREA - DOT_SIZE) / 2);

    Device* i2c = findGroveI2cDevice();
    if (!i2c) {
        lv_label_set_text(self->lblXY_, "grove0_i2c not found");
        return;
    }

    if (self->unit_.begin(i2c)) {
        self->usingPaHub_ = false;
    } else if (self->hub_.begin(i2c)) {
        self->usingPaHub_ = true;
        bool found = false;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !found; ch++) {
            self->hub_.select(ch);
            if (self->unit_.begin(i2c)) found = true;
        }
        if (!found) {
            self->hub_.deselect();
            lv_label_set_text(self->lblXY_, "Joystick2 not found");
            return;
        }
    } else {
        lv_label_set_text(self->lblXY_, "Joystick2 not found");
        return;
    }

    self->timer_ = lv_timer_create(onTimer, 50, self);
    update(self);
}

void testUnitJoystick2Stop(TestUnitJoystick2* self) {
    if (self->timer_) { lv_timer_delete(self->timer_); self->timer_ = nullptr; }
    selectIfNeeded(self);
    if (self->unit_.isPresent()) self->unit_.setLed(0x000000);
    if (self->usingPaHub_ && self->hub_.isPresent()) self->hub_.deselect();
    self->lblXY_ = self->lblButton_ = self->dot_ = self->joyCont_ = nullptr;
}
