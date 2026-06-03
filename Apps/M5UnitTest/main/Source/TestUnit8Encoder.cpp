#include "TestUnit8Encoder.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>
#include <cstring>


void TestUnit8Encoder::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;
    memset(counters_,  0, sizeof(counters_));
    memset(ledColors_, 0, sizeof(ledColors_));

    createToolbar(parent, handle, "8Encoder");
    createBanner(parent, "8Encoder", "I2C", COLOR_I2C);

    int numCols = uiW() >= 800 ? 4 : (uiW() >= 200 ? 2 : 1);
    int dotSz   = (int)(uiShortSide() / 60);
    if (dotSz < 8)  dotSz = 8;
    if (dotSz > 20) dotSz = 20;

    const lv_font_t* fnt = lvgl_get_text_font(uiFont());

    // Status label shown when not connected — sits above the grid at full width
    lblStatus_ = lv_label_create(parent);
    lv_obj_set_style_text_font(lblStatus_, fnt, 0);
    lv_obj_set_width(lblStatus_, LV_PCT(100));
    lv_obj_set_style_pad_hor(lblStatus_, uiPad(), 0);
    lv_label_set_text(lblStatus_, "");

    lv_obj_t* grid = lv_obj_create(parent);
    lv_obj_set_width(grid, LV_PCT(100));
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_layout(grid, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(grid, uiPad(), 0);
    lv_obj_set_style_pad_row(grid, uiRowGap(), 0);
    lv_obj_set_style_pad_column(grid, uiPad(), 0);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);

    auto makeCard = [&](lv_obj_t* parent) -> lv_obj_t* {
        lv_obj_t* card = lv_obj_create(parent);
        lv_obj_set_width(card, numCols == 4 ? LV_PCT(23) : (numCols == 2 ? LV_PCT(48) : LV_PCT(100)));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_layout(card, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, uiPad() / 2, 0);
        lv_obj_set_style_pad_column(card, uiRowGap(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        return card;
    };
    auto makeDot = [&](lv_obj_t* parent) -> lv_obj_t* {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_set_size(dot, dotSz, dotSz);
        lv_obj_set_style_radius(dot, dotSz / 2, 0);
        lv_obj_set_style_bg_color(dot, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(dot, 0, 0);
        lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        return dot;
    };

    for (int i = 0; i < 8; i++) {
        lv_obj_t* card = makeCard(grid);

        lv_obj_t* num = lv_label_create(card);
        lv_label_set_text_fmt(num, "E%d:", i + 1);
        lv_obj_set_style_text_font(num, fnt, 0);
        lv_obj_set_width(num, LV_SIZE_CONTENT);

        lblCounters_[i] = lv_label_create(card);
        lv_label_set_text(lblCounters_[i], "0");
        lv_obj_set_style_text_font(lblCounters_[i], fnt, 0);
        lv_obj_set_flex_grow(lblCounters_[i], 1);

        dotButtons_[i] = makeDot(card);
    }

    // Switch row
    lv_obj_t* swCard = makeCard(grid);
    lv_obj_set_width(swCard, LV_PCT(100));
    lv_obj_t* swLbl = lv_label_create(swCard);
    lv_label_set_text(swLbl, "SW:");
    lv_obj_set_style_text_font(swLbl, fnt, 0);
    lv_obj_set_width(swLbl, LV_SIZE_CONTENT);
    lblSwitch_ = lv_label_create(swCard);
    lv_label_set_text(lblSwitch_, "off");
    lv_obj_set_style_text_font(lblSwitch_, fnt, 0);
    lv_obj_set_flex_grow(lblSwitch_, 1);
    dotSwitch_ = makeDot(swCard);

    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) {
        lv_label_set_text(lblStatus_, "i2c1 not found");
        return;
    }

    if (enc_.begin(i2c)) {
        usingPaHub_ = false;
    } else if (hub_.begin(i2c)) {
        usingPaHub_ = true;
        bool found = false;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !found; ch++) {
            hub_.select(ch);
            if (enc_.begin(i2c)) found = true;
        }
        if (!found) {
            hub_.deselect();
            lv_label_set_text(lblStatus_, "8Encoder not found");
            return;
        }
    } else {
        lv_label_set_text(lblStatus_, "8Encoder not found");
        return;
    }

    timer_ = lv_timer_create(onTimer, 50, this);
    update();
}

void TestUnit8Encoder::onStop() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    selectIfNeeded();
    if (enc_.isPresent()) enc_.setAllLeds(0x000000);
    if (usingPaHub_ && hub_.isPresent()) hub_.deselect();
    lblStatus_ = nullptr;
    memset(lblCounters_, 0, sizeof(lblCounters_));
    memset(dotButtons_,  0, sizeof(dotButtons_));
    lblSwitch_ = dotSwitch_ = nullptr;
}

void TestUnit8Encoder::onTimer(lv_timer_t* t) {
    static_cast<TestUnit8Encoder*>(lv_timer_get_user_data(t))->update();
}

void TestUnit8Encoder::selectIfNeeded() {
    if (usingPaHub_ && hub_.isPresent())
        hub_.select(hub_.currentChannel());
}

void TestUnit8Encoder::update() {
    selectIfNeeded();
    if (!enc_.isPresent()) return;
    int32_t deltas[8];
    uint8_t buttons[8];
    if (!enc_.readAll(deltas, buttons)) return;

    for (int i = 0; i < 8; i++) {
        counters_[i] += deltas[i];
        lv_label_set_text_fmt(lblCounters_[i], "%ld", (long)counters_[i]);

        lv_color_t dotCol = buttons[i] ? lv_color_hex(0x00DD44) : lv_color_hex(0x333333);
        lv_obj_set_style_bg_color(dotButtons_[i], dotCol, 0);

        // Encoder LED: hue cycles with counter position
        uint32_t hue = (uint32_t)((counters_[i] % 360 + 360) % 360);
        lv_color_t c = lv_color_hsv_to_rgb((uint16_t)hue, 100, 78);
        lv_color32_t c32 = lv_color_to_32(c, LV_OPA_COVER);
        ledColors_[i] = ((uint32_t)c32.red << 16) | ((uint32_t)c32.green << 8) | c32.blue;
    }

    // Switch (index 8): green=on, dark gray=off; skip update on I2C error
    bool sw = false;
    if (enc_.readSwitch(sw)) {
        lv_label_set_text(lblSwitch_, sw ? "on" : "off");
        lv_obj_set_style_bg_color(dotSwitch_, lv_color_hex(sw ? 0x00DD44 : 0x333333), 0);
        ledColors_[8] = sw ? 0x00DD44 : 0x000000;
    }

    enc_.flushLeds(ledColors_);
}
