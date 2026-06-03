#include "TestUnitPaHub.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/drivers/i2c_controller.h>
#include <tactility/lvgl_fonts.h>

void TestUnitPaHub::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;

    createToolbar(parent, handle, "PaHub");
    createBanner(parent, "PaHub", "I2C", COLOR_I2C);

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

    for (int i = 0; i < CH_COUNT; i++) {
        lv_obj_t* btn = lv_button_create(btnRow);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_style_pad_hor(btn, pad, 0);
        lv_obj_set_style_pad_ver(btn, gap, 0);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, onChannelBtn, LV_EVENT_CLICKED, this);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "CH%d", i);
        lv_obj_set_style_text_font(lbl, fnt, 0);
        lv_obj_align(lbl, LV_ALIGN_CENTER, 0, 0);
        btnCh_[i] = btn;
    }

    lblStatus_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblStatus_, fnt, 0);
    lv_label_set_text(lblStatus_, "Select a channel to probe");

    for (int i = 0; i < CH_COUNT; i++) {
        lblCh_[i] = lv_label_create(cont);
        lv_obj_set_style_text_font(lblCh_[i], fnt, 0);
        lv_label_set_text_fmt(lblCh_[i], "CH%d: -", i);
    }

    Device* i2c = device_find_by_name("i2c1");
    if (!i2c || !hub_.begin(i2c)) {
        lv_label_set_text(lblStatus_, "PaHub not found");
        return;
    }

    lv_label_set_text(lblStatus_, "PaHub ready - tap channel to probe");

    timer_ = lv_timer_create(onTimer, 1000, this);
}

void TestUnitPaHub::onStop() {
    if (timer_) { lv_timer_delete(timer_); timer_ = nullptr; }
    if (hub_.isPresent()) hub_.deselect();
    lblStatus_ = nullptr;
    for (int i = 0; i < CH_COUNT; i++) { btnCh_[i] = nullptr; lblCh_[i] = nullptr; }
}

void TestUnitPaHub::onChannelBtn(lv_event_t* e) {
    auto* self = static_cast<TestUnitPaHub*>(lv_event_get_user_data(e));
    int ch = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    self->selChannel_ = ch;
    self->probeSelected();
}

void TestUnitPaHub::onTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitPaHub*>(lv_timer_get_user_data(t));
    if (self->selChannel_ >= 0) self->probeSelected();
}

void TestUnitPaHub::probeSelected() {
    if (!hub_.isPresent() || selChannel_ < 0 || selChannel_ >= CH_COUNT) return;

    hub_.select((uint8_t)selChannel_);

    // Scan I2C addresses 0x08-0x77 on this channel
    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) {
        lv_label_set_text_fmt(lblCh_[selChannel_], "CH%d: i2c1 not found", selChannel_);
        hub_.deselect();
        return;
    }
    char found[256] = "Found: ";
    bool any = false;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
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

    lv_label_set_text_fmt(lblCh_[selChannel_], "CH%d: %s", selChannel_, found);
    lv_label_set_text_fmt(lblStatus_, "Probed CH%d", selChannel_);

    hub_.deselect();
}
