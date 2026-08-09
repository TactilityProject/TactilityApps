#include "TestUnitPaHub.h"
#include "M5UnitTest.h"
#include "GroveLookup.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/drivers/i2c_controller.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/fonts.h>
#include <cstring>
#include <cstdio>

namespace {

void probeSelected(TestUnitPaHub* self) {
    if (!self->hub_.isPresent() || self->selChannel_ < 0 || self->selChannel_ >= TestUnitPaHub::CH_COUNT) return;

    self->hub_.select((uint8_t)self->selChannel_);

    // Probe known unit addresses only (full-range scan can wedge the ESP-IDF i2c_master bus FSM)
    Device* i2c = findGroveI2cDevice();
    if (!i2c) {
        lv_label_set_text_fmt(self->lblCh_[self->selChannel_], "CH%d: grove0_i2c not found", self->selChannel_);
        self->hub_.deselect();
        return;
    }
    char found[256] = "Found: ";
    bool any = false;
    for (uint8_t addr : KNOWN_UNIT_ADDRS) {
        if (i2c_controller_has_device_at_address(i2c, addr,
            pdMS_TO_TICKS(10)) == ERROR_NONE) {
            size_t remaining = sizeof(found) - strlen(found) - 1;
            if (remaining < 7) {
                strncat(found, "...", remaining);
                break;
            }
            char hex[8];
            snprintf(hex, sizeof(hex), "0x%02X ", addr);
            strncat(found, hex, remaining);
            any = true;
        }
    }
    if (!any) strcpy(found, "No devices found");

    lv_label_set_text_fmt(self->lblCh_[self->selChannel_], "CH%d: %s", self->selChannel_, found);
    lv_label_set_text_fmt(self->lblStatus_, "Probed CH%d", self->selChannel_);

    self->hub_.deselect();
}

void onChannelBtn(lv_event_t* e) {
    auto* self = static_cast<TestUnitPaHub*>(lv_event_get_user_data(e));
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    self->selChannel_ = ch;
    probeSelected(self);
}

void onTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitPaHub*>(lv_timer_get_user_data(t));
    if (window_manager_get_state(self->app_->window) != WINDOW_STATE_GRANTED) return;
    if (self->selChannel_ >= 0) probeSelected(self);
}

} // namespace

void testUnitPaHubStart(TestUnitPaHub* self, lv_obj_t* parent, Context* app) {
    self->app_ = app;

    testViewCreateToolbar(parent, app, "PaHub");
    testViewCreateBanner(parent, "PaHub", "I2C", COLOR_I2C);

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
    int pad = uiPad();
    int gap = uiRowGap();

    // Row of channel buttons — flex-grow so they share space evenly at any width
    lv_obj_t* btnRow = lv_obj_create(cont);
    lv_obj_set_width(btnRow, LV_PCT(100));
    lv_obj_set_height(btnRow, LV_SIZE_CONTENT);
    lv_obj_set_layout(btnRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btnRow, gap, 0);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);

    for (int i = 0; i < TestUnitPaHub::CH_COUNT; i++) {
        lv_obj_t* btn = lv_button_create(btnRow);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_pad_hor(btn, pad, 0);
        lv_obj_set_style_pad_ver(btn, gap, 0);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, onChannelBtn, LV_EVENT_CLICKED, self);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "CH%d", i);
        lv_obj_set_style_text_font(lbl, fnt, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        self->btnCh_[i] = btn;
    }

    self->lblStatus_ = lv_label_create(cont);
    lv_obj_set_style_text_font(self->lblStatus_, fnt, 0);
    lv_label_set_text(self->lblStatus_, "Select a channel to probe");

    for (int i = 0; i < TestUnitPaHub::CH_COUNT; i++) {
        self->lblCh_[i] = lv_label_create(cont);
        lv_obj_set_style_text_font(self->lblCh_[i], fnt, 0);
        lv_label_set_text_fmt(self->lblCh_[i], "CH%d: -", i);
    }

    Device* i2c = findGroveI2cDevice();
    if (!i2c || !self->hub_.begin(i2c)) {
        lv_label_set_text(self->lblStatus_, "PaHub not found");
        return;
    }

    lv_label_set_text(self->lblStatus_, "PaHub ready - tap channel to probe");

    self->timer_ = lv_timer_create(onTimer, 1000, self);
}

void testUnitPaHubStop(TestUnitPaHub* self) {
    if (self->timer_) { lv_timer_delete(self->timer_); self->timer_ = nullptr; }
    if (self->hub_.isPresent()) self->hub_.deselect();
    self->lblStatus_ = nullptr;
    for (int i = 0; i < TestUnitPaHub::CH_COUNT; i++) { self->btnCh_[i] = nullptr; self->lblCh_[i] = nullptr; }
}
