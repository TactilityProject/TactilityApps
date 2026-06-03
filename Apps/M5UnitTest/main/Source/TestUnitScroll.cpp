#include "TestUnitScroll.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>

void TestUnitScroll::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;

    createToolbar(parent, handle, "Scroll");
    createBanner(parent, "Scroll", "I2C", COLOR_I2C);

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, uiPad(), 0);
    lv_obj_set_style_pad_row(cont, uiRowGap(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    const lv_font_t* fnt = lvgl_get_text_font(uiFont());

    lblCounter_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblCounter_, lvgl_get_text_font(uiW() < 200 ? FONT_SIZE_DEFAULT : FONT_SIZE_LARGE), 0);

    lblButton_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblButton_, fnt, 0);

    lblLed_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblLed_, fnt, 0);

    // RGB sliders
    auto makeSlider = [&](const char* name) -> lv_obj_t* {
        lv_obj_t* row = lv_obj_create(cont);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, name);
        lv_obj_set_style_text_font(lbl, fnt, 0);
        lv_obj_set_width(lbl, LV_SIZE_CONTENT);
        lv_obj_t* sl = lv_slider_create(row);
        lv_slider_set_range(sl, 0, 255);
        lv_slider_set_value(sl, 0, LV_ANIM_OFF);
        lv_obj_set_flex_grow(sl, 1);
        lv_obj_add_event_cb(sl, onSliderChanged, LV_EVENT_VALUE_CHANGED, this);
        return sl;
    };
    sliderR_ = makeSlider("R");
    sliderG_ = makeSlider("G");
    sliderB_ = makeSlider("B");
    lv_label_set_text(lblLed_, "LED: #000000");

    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) {
        lv_label_set_text(lblCounter_, "i2c1 not found");
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
            lv_label_set_text(lblCounter_, "Scroll not found");
            return;
        }
    } else {
        lv_label_set_text(lblCounter_, "Scroll not found");
        return;
    }

    timer_ = lv_timer_create(onTimer, 50, this);
    update();
}

void TestUnitScroll::onStop() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    selectIfNeeded();
    if (unit_.isPresent()) unit_.setLed(0x000000);
    if (usingPaHub_ && hub_.isPresent()) hub_.deselect();
    lblCounter_ = lblButton_ = lblLed_ = nullptr;
    sliderR_ = sliderG_ = sliderB_ = nullptr;
}

void TestUnitScroll::selectIfNeeded() {
    if (usingPaHub_ && hub_.isPresent())
        hub_.select(hub_.currentChannel());
}

void TestUnitScroll::onTimer(lv_timer_t* t) {
    static_cast<TestUnitScroll*>(lv_timer_get_user_data(t))->update();
}

void TestUnitScroll::onSliderChanged(lv_event_t* e) {
    static_cast<TestUnitScroll*>(lv_event_get_user_data(e))->updateLedFromSliders();
}

void TestUnitScroll::update() {
    selectIfNeeded();
    if (!unit_.isPresent()) return;
    counter_ -= unit_.readDelta();
    lv_label_set_text_fmt(lblCounter_, "Counter: %ld", (long)counter_);
    lv_label_set_text_fmt(lblButton_, "Button: %s", unit_.isPressed() ? "PRESSED" : "released");
}

void TestUnitScroll::updateLedFromSliders() {
    selectIfNeeded();
    if (!unit_.isPresent()) return;
    uint32_t r = (uint32_t)lv_slider_get_value(sliderR_);
    uint32_t g = (uint32_t)lv_slider_get_value(sliderG_);
    uint32_t b = (uint32_t)lv_slider_get_value(sliderB_);
    uint32_t rgb = (r << 16) | (g << 8) | b;
    unit_.setLed(rgb);
    lv_label_set_text_fmt(lblLed_, "LED: #%06lX", (unsigned long)rgb);
}
