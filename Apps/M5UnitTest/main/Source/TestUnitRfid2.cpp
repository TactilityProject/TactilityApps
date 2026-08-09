#include "TestUnitRfid2.h"
#include "M5UnitTest.h"
#include "GroveLookup.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/fonts.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

void selectIfNeeded(TestUnitRfid2* self) {
    if (self->usingPaHub_ && self->hub_.isPresent())
        self->hub_.select(self->hub_.currentChannel());
}

void showCard(TestUnitRfid2* self, const UnitRfid2::Uid& uid) {
    self->lastUid_   = uid;
    self->cardType_  = self->unit_.getCardType(uid);
    self->cardShown_ = true;

    // UID string
    char uidBuf[40] = "UID: ";
    int pos = 5;
    uint8_t size = (uid.size <= 10) ? uid.size : 10;
    for (uint8_t i = 0; i < size; i++)
        pos += snprintf(uidBuf + pos, sizeof(uidBuf) - (size_t)pos, "%02X ", uid.bytes[i]);
    lv_label_set_text(self->lblUid_, uidBuf);

    char typeBuf[64];
    snprintf(typeBuf, sizeof(typeBuf), "Type: %s", self->unit_.cardTypeName(self->cardType_));
    lv_label_set_text(self->lblType_, typeBuf);

    char sakBuf[40];
    snprintf(sakBuf, sizeof(sakBuf), "SAK: %02X  ATQA: %02X %02X",
             uid.sak, uid.atqa[0], uid.atqa[1]);
    lv_label_set_text(self->lblSak_, sakBuf);

    lv_obj_add_flag(self->idleGroup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(self->cardGroup_, LV_OBJ_FLAG_HIDDEN);
    self->unit_.haltCard();
}

void onTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitRfid2*>(lv_timer_get_user_data(t));
    if (window_manager_get_state(self->app_->window) != WINDOW_STATE_GRANTED) return;
    if (self->cardShown_) return;

    selectIfNeeded(self);
    if (!self->unit_.isPresent()) return;

    UnitRfid2::Uid uid = {};
    if (self->unit_.readCard(&uid))
        showCard(self, uid);
}

void onPulseTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitRfid2*>(lv_timer_get_user_data(t));
    if (window_manager_get_state(self->app_->window) != WINDOW_STATE_GRANTED) return;
    if (self->cardShown_ || !self->circle_) return;

    self->pulseOpa_ = static_cast<uint8_t>(self->pulseOpa_ + self->pulseDir_);
    if (self->pulseOpa_ >= 255) {
        self->pulseOpa_ = 255;
        self->pulseDir_ = -8;
    } else if (self->pulseOpa_ <= 180) {
        self->pulseOpa_ = 180;
        self->pulseDir_ =  8;
    }
    lv_obj_set_style_bg_opa(self->circle_, self->pulseOpa_, 0);
}

void onClear(lv_event_t* e) {
    auto* self = static_cast<TestUnitRfid2*>(lv_event_get_user_data(e));
    self->cardShown_ = false;
    lv_obj_remove_flag(self->idleGroup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(self->cardGroup_,   LV_OBJ_FLAG_HIDDEN);
}

} // namespace

void testUnitRfid2Start(TestUnitRfid2* self, lv_obj_t* parent, Context* app) {
    self->app_ = app;

    testViewCreateToolbar(parent, app, "RFID 2");
    testViewCreateBanner(parent, "RFID 2", "I2C", COLOR_I2C);

    int  pad    = uiPad();
    int  rowGap = uiRowGap();
    auto font   = uiFont();
    bool wide   = uiW() >= 240;

    // Compute circle diameter: min(availW, availH - toolbar) * 2/3, clamped to 300
    lv_coord_t availW = uiW();
    lv_coord_t availH = uiH() - 50;
    lv_coord_t diam   = static_cast<lv_coord_t>(std::min(availW, availH) * 2 / 3);
    if (diam > 300) diam = 300;

    // Content container - flex column, fills remaining space, centered
    lv_obj_t* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_layout(content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(content, pad, 0);
    lv_obj_set_style_pad_row(content, rowGap, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);

    // Idle group
    self->idleGroup_ = lv_obj_create(content);
    lv_obj_set_width(self->idleGroup_, LV_SIZE_CONTENT);
    lv_obj_set_height(self->idleGroup_, LV_SIZE_CONTENT);
    lv_obj_set_layout(self->idleGroup_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(self->idleGroup_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(self->idleGroup_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(self->idleGroup_, 0, 0);
    lv_obj_set_style_pad_row(self->idleGroup_, rowGap, 0);
    lv_obj_set_style_bg_opa(self->idleGroup_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(self->idleGroup_, 0, 0);

    // Green circle
    self->circle_ = lv_obj_create(self->idleGroup_);
    lv_obj_set_size(self->circle_, diam, diam);
    lv_obj_set_style_radius(self->circle_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(self->circle_, LV_COLOR_MAKE(0x20, 0xC0, 0x50), 0);
    lv_obj_set_style_bg_opa(self->circle_, self->pulseOpa_, 0);
    lv_obj_set_style_border_width(self->circle_, 0, 0);
    lv_obj_remove_flag(self->circle_, LV_OBJ_FLAG_SCROLLABLE);

    // "Tap a tag/card..." label
    lv_obj_t* tapLabel = lv_label_create(self->idleGroup_);
    lv_obj_set_style_text_font(tapLabel, lvgl_get_text_font(font), 0);
    lv_label_set_text(tapLabel, "Tap a tag/card...");

    // ── Card info group ───────────────────────────────────────────────────────
    self->cardGroup_ = lv_obj_create(content);
    lv_obj_set_width(self->cardGroup_, LV_PCT(100));
    lv_obj_set_height(self->cardGroup_, LV_SIZE_CONTENT);
    lv_obj_set_layout(self->cardGroup_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(self->cardGroup_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(self->cardGroup_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(self->cardGroup_, pad, 0);
    lv_obj_set_style_pad_row(self->cardGroup_, rowGap, 0);
    lv_obj_set_style_bg_opa(self->cardGroup_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(self->cardGroup_, 0, 0);
    lv_obj_add_flag(self->cardGroup_, LV_OBJ_FLAG_HIDDEN);

    self->lblUid_ = lv_label_create(self->cardGroup_);
    lv_obj_set_style_text_font(self->lblUid_,
        lvgl_get_text_font(wide ? FONT_SIZE_LARGE : FONT_SIZE_DEFAULT), 0);
    lv_label_set_text(self->lblUid_, "");

    self->lblType_ = lv_label_create(self->cardGroup_);
    lv_obj_set_style_text_font(self->lblType_, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_label_set_text(self->lblType_, "");

    self->lblSak_ = lv_label_create(self->cardGroup_);
    lv_obj_set_style_text_font(self->lblSak_, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_label_set_text(self->lblSak_, "");

    lv_obj_t* btnClear = lv_button_create(self->cardGroup_);
    lv_obj_set_style_pad_hor(btnClear, pad * 2, 0);
    lv_obj_set_style_pad_ver(btnClear, rowGap, 0);
    lv_obj_add_event_cb(btnClear, onClear, LV_EVENT_CLICKED, self);
    lv_obj_t* btnLbl = lv_label_create(btnClear);
    lv_obj_set_style_text_font(btnLbl, lvgl_get_text_font(font), 0);
    lv_label_set_text(btnLbl, "Clear");

    // ── Device discovery ─────────────────────────────────────────────────────
    Device* i2c = findGroveI2cDevice();
    if (!i2c) return;

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
            return;
        }
    } else {
        return;
    }

    self->timer_      = lv_timer_create(onTimer,      100, self);
    self->pulseTimer_ = lv_timer_create(onPulseTimer,  60, self);
}

void testUnitRfid2Stop(TestUnitRfid2* self) {
    if (self->timer_)      { lv_timer_delete(self->timer_);      self->timer_      = nullptr; }
    if (self->pulseTimer_) { lv_timer_delete(self->pulseTimer_);  self->pulseTimer_ = nullptr; }
    if (self->usingPaHub_ && self->hub_.isPresent()) self->hub_.deselect();

    self->cardShown_ = false;
    self->idleGroup_ = self->circle_ = nullptr;
    self->cardGroup_ = self->lblUid_ = self->lblType_ = self->lblSak_ = nullptr;
}
