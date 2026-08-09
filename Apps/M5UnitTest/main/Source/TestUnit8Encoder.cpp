#include "TestUnit8Encoder.h"
#include "M5UnitTest.h"
#include "GroveLookup.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/fonts.h>
#include <cstring>

namespace {

void selectIfNeeded(TestUnit8Encoder* self) {
    if (self->usingPaHub_ && self->hub_.isPresent())
        self->hub_.select(self->hub_.currentChannel());
}

void update(TestUnit8Encoder* self) {
    selectIfNeeded(self);
    if (!self->enc_.isPresent()) return;
    int32_t deltas[8];
    uint8_t buttons[8];
    if (!self->enc_.readAll(deltas, buttons)) return;

    for (int i = 0; i < 8; i++) {
        self->counters_[i] += deltas[i];
        lv_label_set_text_fmt(self->lblCounters_[i], "%ld", (long)self->counters_[i]);

        lv_color_t dotCol = buttons[i] ? lv_color_hex(0x00DD44) : lv_color_hex(0x333333);
        lv_obj_set_style_bg_color(self->dotButtons_[i], dotCol, 0);

        // Encoder LED: hue cycles with counter position
        uint32_t hue = (uint32_t)((self->counters_[i] % 360 + 360) % 360);
        lv_color_t c = lv_color_hsv_to_rgb((uint16_t)hue, 100, 78);
        lv_color32_t c32 = lv_color_to_32(c, LV_OPA_COVER);
        self->ledColors_[i] = ((uint32_t)c32.red << 16) | ((uint32_t)c32.green << 8) | c32.blue;
    }

    // Switch (index 8): green=on, dark gray=off; skip update on I2C error
    bool sw = false;
    if (self->enc_.readSwitch(sw)) {
        lv_label_set_text(self->lblSwitch_, sw ? "on" : "off");
        lv_obj_set_style_bg_color(self->dotSwitch_, lv_color_hex(sw ? 0x00DD44 : 0x333333), 0);
        self->ledColors_[8] = sw ? 0x00DD44 : 0x000000;
    }

    self->enc_.flushLeds(self->ledColors_);
}

void onTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnit8Encoder*>(lv_timer_get_user_data(t));
    if (window_manager_get_state(self->app_->window) != WINDOW_STATE_GRANTED) return;
    update(self);
}

} // namespace

void testUnit8EncoderStart(TestUnit8Encoder* self, lv_obj_t* parent, Context* app) {
    self->app_ = app;
    memset(self->counters_,  0, sizeof(self->counters_));
    memset(self->ledColors_, 0, sizeof(self->ledColors_));

    testViewCreateToolbar(parent, app, "8Encoder");
    testViewCreateBanner(parent, "8Encoder", "I2C", COLOR_I2C);

    int numCols = uiW() >= 800 ? 4 : (uiW() >= 200 ? 2 : 1);
    int dotSz   = (int)(uiShortSide() / 60);
    if (dotSz < 8)  dotSz = 8;
    if (dotSz > 20) dotSz = 20;

    const lv_font_t* fnt = lvgl_get_text_font(uiFont());

    // Status label shown when not connected — sits above the grid at full width
    self->lblStatus_ = lv_label_create(parent);
    lv_obj_set_style_text_font(self->lblStatus_, fnt, 0);
    lv_obj_set_width(self->lblStatus_, LV_PCT(100));
    lv_obj_set_style_pad_hor(self->lblStatus_, uiPad(), 0);
    lv_label_set_text(self->lblStatus_, "");

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

        self->lblCounters_[i] = lv_label_create(card);
        lv_label_set_text(self->lblCounters_[i], "0");
        lv_obj_set_style_text_font(self->lblCounters_[i], fnt, 0);
        lv_obj_set_flex_grow(self->lblCounters_[i], 1);

        self->dotButtons_[i] = makeDot(card);
    }

    // Switch row
    lv_obj_t* swCard = makeCard(grid);
    lv_obj_set_width(swCard, LV_PCT(100));
    lv_obj_t* swLbl = lv_label_create(swCard);
    lv_label_set_text(swLbl, "SW:");
    lv_obj_set_style_text_font(swLbl, fnt, 0);
    lv_obj_set_width(swLbl, LV_SIZE_CONTENT);
    self->lblSwitch_ = lv_label_create(swCard);
    lv_label_set_text(self->lblSwitch_, "off");
    lv_obj_set_style_text_font(self->lblSwitch_, fnt, 0);
    lv_obj_set_flex_grow(self->lblSwitch_, 1);
    self->dotSwitch_ = makeDot(swCard);

    Device* i2c = findGroveI2cDevice();
    if (!i2c) {
        lv_label_set_text(self->lblStatus_, "grove0_i2c not found");
        return;
    }

    if (self->enc_.begin(i2c)) {
        self->usingPaHub_ = false;
    } else if (self->hub_.begin(i2c)) {
        self->usingPaHub_ = true;
        bool found = false;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !found; ch++) {
            self->hub_.select(ch);
            if (self->enc_.begin(i2c)) found = true;
        }
        if (!found) {
            self->hub_.deselect();
            lv_label_set_text(self->lblStatus_, "8Encoder not found");
            return;
        }
    } else {
        lv_label_set_text(self->lblStatus_, "8Encoder not found");
        return;
    }

    self->timer_ = lv_timer_create(onTimer, 50, self);
    update(self);
}

void testUnit8EncoderStop(TestUnit8Encoder* self) {
    if (self->timer_) { lv_timer_delete(self->timer_); self->timer_ = nullptr; }
    selectIfNeeded(self);
    if (self->enc_.isPresent()) self->enc_.setAllLeds(0x000000);
    if (self->usingPaHub_ && self->hub_.isPresent()) self->hub_.deselect();
    self->lblStatus_ = nullptr;
    memset(self->lblCounters_, 0, sizeof(self->lblCounters_));
    memset(self->dotButtons_,  0, sizeof(self->dotButtons_));
    self->lblSwitch_ = self->dotSwitch_ = nullptr;
}
