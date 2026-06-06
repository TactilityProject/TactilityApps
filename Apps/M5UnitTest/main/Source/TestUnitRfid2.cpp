#include "TestUnitRfid2.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>
#include <tt_lvgl_toolbar.h>
#include <algorithm>
#include <cstdio>
#include <cstring>

// ---------------------------------------------------------------------------
// onStart
// ---------------------------------------------------------------------------

void TestUnitRfid2::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;

    createToolbar(parent, handle, "RFID 2");
    createBanner(parent, "RFID 2", "I2C", COLOR_I2C);

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
    idleGroup_ = lv_obj_create(content);
    lv_obj_set_width(idleGroup_, LV_SIZE_CONTENT);
    lv_obj_set_height(idleGroup_, LV_SIZE_CONTENT);
    lv_obj_set_layout(idleGroup_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(idleGroup_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(idleGroup_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(idleGroup_, 0, 0);
    lv_obj_set_style_pad_row(idleGroup_, rowGap, 0);
    lv_obj_set_style_bg_opa(idleGroup_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(idleGroup_, 0, 0);

    // Green circle
    circle_ = lv_obj_create(idleGroup_);
    lv_obj_set_size(circle_, diam, diam);
    lv_obj_set_style_radius(circle_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle_, LV_COLOR_MAKE(0x20, 0xC0, 0x50), 0);
    lv_obj_set_style_bg_opa(circle_, pulseOpa_, 0);
    lv_obj_set_style_border_width(circle_, 0, 0);
    lv_obj_remove_flag(circle_, LV_OBJ_FLAG_SCROLLABLE);

    // "Tap a tag/card..." label
    lv_obj_t* tapLabel = lv_label_create(idleGroup_);
    lv_obj_set_style_text_font(tapLabel, lvgl_get_text_font(font), 0);
    lv_label_set_text(tapLabel, "Tap a tag/card...");

    // ── Card info group ───────────────────────────────────────────────────────
    cardGroup_ = lv_obj_create(content);
    lv_obj_set_width(cardGroup_, LV_PCT(100));
    lv_obj_set_height(cardGroup_, LV_SIZE_CONTENT);
    lv_obj_set_layout(cardGroup_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cardGroup_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cardGroup_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(cardGroup_, pad, 0);
    lv_obj_set_style_pad_row(cardGroup_, rowGap, 0);
    lv_obj_set_style_bg_opa(cardGroup_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cardGroup_, 0, 0);
    lv_obj_add_flag(cardGroup_, LV_OBJ_FLAG_HIDDEN);

    lblUid_ = lv_label_create(cardGroup_);
    lv_obj_set_style_text_font(lblUid_,
        lvgl_get_text_font(wide ? FONT_SIZE_LARGE : FONT_SIZE_DEFAULT), 0);
    lv_label_set_text(lblUid_, "");

    lblType_ = lv_label_create(cardGroup_);
    lv_obj_set_style_text_font(lblType_, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_label_set_text(lblType_, "");

    lblSak_ = lv_label_create(cardGroup_);
    lv_obj_set_style_text_font(lblSak_, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_label_set_text(lblSak_, "");

    lv_obj_t* btnClear = lv_button_create(cardGroup_);
    lv_obj_set_style_pad_hor(btnClear, pad * 2, 0);
    lv_obj_set_style_pad_ver(btnClear, rowGap, 0);
    lv_obj_add_event_cb(btnClear, onClear, LV_EVENT_CLICKED, this);
    lv_obj_t* btnLbl = lv_label_create(btnClear);
    lv_obj_set_style_text_font(btnLbl, lvgl_get_text_font(font), 0);
    lv_label_set_text(btnLbl, "Clear");

    // ── Device discovery ─────────────────────────────────────────────────────
    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) return;

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
            return;
        }
    } else {
        return;
    }

    timer_      = lv_timer_create(onTimer,      100, this);
    pulseTimer_ = lv_timer_create(onPulseTimer,  60, this);
}

// ---------------------------------------------------------------------------
// onStop
// ---------------------------------------------------------------------------

void TestUnitRfid2::onStop() {
    if (timer_)      { lv_timer_delete(timer_);      timer_      = nullptr; }
    if (pulseTimer_) { lv_timer_delete(pulseTimer_);  pulseTimer_ = nullptr; }
    if (usingPaHub_ && hub_.isPresent()) hub_.deselect();

    cardShown_ = false;
    idleGroup_ = circle_ = nullptr;
    cardGroup_ = lblUid_ = lblType_ = lblSak_ = nullptr;
}

// ---------------------------------------------------------------------------
// PaHub helper
// ---------------------------------------------------------------------------

void TestUnitRfid2::selectIfNeeded() {
    if (usingPaHub_ && hub_.isPresent())
        hub_.select(hub_.currentChannel());
}

// ---------------------------------------------------------------------------
// showCard
// ---------------------------------------------------------------------------

void TestUnitRfid2::showCard(const UnitRfid2::Uid& uid) {
    lastUid_   = uid;
    cardType_  = unit_.getCardType(uid);
    cardShown_ = true;

    // UID string
    char uidBuf[40] = "UID: ";
    int pos = 5;
    uint8_t size = (uid.size <= 10) ? uid.size : 10;
    for (uint8_t i = 0; i < size; i++)
        pos += snprintf(uidBuf + pos, sizeof(uidBuf) - (size_t)pos, "%02X ", uid.bytes[i]);
    lv_label_set_text(lblUid_, uidBuf);

    char typeBuf[64];
    snprintf(typeBuf, sizeof(typeBuf), "Type: %s", unit_.cardTypeName(cardType_));
    lv_label_set_text(lblType_, typeBuf);

    char sakBuf[40];
    snprintf(sakBuf, sizeof(sakBuf), "SAK: %02X  ATQA: %02X %02X",
             uid.sak, uid.atqa[0], uid.atqa[1]);
    lv_label_set_text(lblSak_, sakBuf);

    lv_obj_add_flag(idleGroup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(cardGroup_, LV_OBJ_FLAG_HIDDEN);
    unit_.haltCard();
}

// ---------------------------------------------------------------------------
// Timer callbacks
// ---------------------------------------------------------------------------

void TestUnitRfid2::onTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitRfid2*>(lv_timer_get_user_data(t));
    if (self->cardShown_) return;

    self->selectIfNeeded();
    if (!self->unit_.isPresent()) return;

    UnitRfid2::Uid uid = {};
    if (self->unit_.readCard(&uid))
        self->showCard(uid);
}

void TestUnitRfid2::onPulseTimer(lv_timer_t* t) {
    auto* self = static_cast<TestUnitRfid2*>(lv_timer_get_user_data(t));
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

// ---------------------------------------------------------------------------
// Clear button callback
// ---------------------------------------------------------------------------

void TestUnitRfid2::onClear(lv_event_t* e) {
    auto* self = static_cast<TestUnitRfid2*>(lv_event_get_user_data(e));
    self->cardShown_ = false;
    lv_obj_remove_flag(self->idleGroup_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(self->cardGroup_,   LV_OBJ_FLAG_HIDDEN);
}
