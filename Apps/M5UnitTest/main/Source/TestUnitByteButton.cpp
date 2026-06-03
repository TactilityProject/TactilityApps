#include "TestUnitByteButton.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>
#include <cstring>

void TestUnitByteButton::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;
    memset(ledColors_,   0, sizeof(ledColors_));
    memset(prevPressed_, 0, sizeof(prevPressed_));

    createToolbar(parent, handle, "ByteButton");
    createBanner(parent, "ByteButton", "I2C", COLOR_I2C);

    int dotSz = (int)(uiShortSide() / 14);
    if (dotSz < 20) dotSz = 20;
    if (dotSz > 64) dotSz = 64;

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, uiRowGap() * 2, 0);
    lv_obj_set_style_pad_all(cont, uiPad(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    // Dot grid - flex wrap row so it auto-adjusts columns
    lv_obj_t* dotGrid = lv_obj_create(cont);
    lv_obj_set_width(dotGrid, LV_PCT(100));
    lv_obj_set_height(dotGrid, LV_SIZE_CONTENT);
    lv_obj_set_layout(dotGrid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(dotGrid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(dotGrid, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(dotGrid, uiRowGap(), 0);
    lv_obj_set_style_pad_column(dotGrid, uiRowGap(), 0);
    lv_obj_set_style_bg_opa(dotGrid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(dotGrid, 0, 0);
    lv_obj_set_style_pad_all(dotGrid, 0, 0);

    for (int i = 0; i < BTN_COUNT; i++) {
        lv_obj_t* sq = lv_obj_create(dotGrid);
        lv_obj_set_size(sq, dotSz, dotSz);
        lv_obj_set_style_radius(sq, 4, 0);
        lv_obj_set_style_bg_color(sq, lv_color_hex(COLOR_OFF), 0);
        lv_obj_set_style_bg_opa(sq, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(sq, lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(sq, 1, 0);
        lv_obj_remove_flag(sq, LV_OBJ_FLAG_SCROLLABLE);
        indicators_[i] = sq;
    }

    lv_obj_t* hint = lv_label_create(cont);
    lv_obj_set_style_text_font(hint, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_label_set_text(hint, "Press buttons - LEDs toggle");

    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) {
        for (int i = 0; i < BTN_COUNT; i++) lv_obj_set_style_bg_color(indicators_[i], lv_color_hex(COLOR_ERROR), 0);
        lv_label_set_text(hint, "i2c1 not found");
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
            for (int i = 0; i < BTN_COUNT; i++) lv_obj_set_style_bg_color(indicators_[i], lv_color_hex(COLOR_ERROR), 0);
            lv_label_set_text(hint, "ByteButton not found");
            return;
        }
    } else {
        for (int i = 0; i < BTN_COUNT; i++) lv_obj_set_style_bg_color(indicators_[i], lv_color_hex(COLOR_ERROR), 0);
        lv_label_set_text(hint, "ByteButton not found");
        return;
    }

    timer_ = lv_timer_create(onTimer, 50, this);
    update();
}

void TestUnitByteButton::onStop() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    selectIfNeeded();
    if (unit_.isPresent()) {
        for (int i = 0; i < BTN_COUNT; i++) unit_.setLed((uint8_t)i, 0x000000);
    }
    if (usingPaHub_ && hub_.isPresent()) hub_.deselect();
    memset(indicators_, 0, sizeof(indicators_));
}

void TestUnitByteButton::selectIfNeeded() {
    if (usingPaHub_ && hub_.isPresent())
        hub_.select(hub_.currentChannel());
}

void TestUnitByteButton::onTimer(lv_timer_t* t) {
    static_cast<TestUnitByteButton*>(lv_timer_get_user_data(t))->update();
}

void TestUnitByteButton::update() {
    selectIfNeeded();
    if (!unit_.isPresent()) return;
    uint8_t mask = unit_.readButtons();
    for (int i = 0; i < BTN_COUNT; i++) {
        bool pressed = (mask >> i) & 0x01;
        // Toggle LED only on rising edge (press, not hold)
        if (pressed && !prevPressed_[i]) {
            ledColors_[i] = (ledColors_[i] == 0) ? COLOR_ON : 0;
            unit_.setLed((uint8_t)i, ledColors_[i]);
        }
        prevPressed_[i] = pressed;
        lv_color_t col = pressed ? lv_color_hex(COLOR_PRESSED) : lv_color_hex(COLOR_OFF);
        lv_obj_set_style_bg_color(indicators_[i], col, 0);
    }
}
