#include "TestUnitLcd.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>

void TestUnitLcd::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;
    rotation_    = 0;
    usingPaHub_  = false;

    createToolbar(parent, handle, "Color LCD");
    createBanner(parent, "Color LCD", "I2C", COLOR_I2C);

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

    lblStatus_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblStatus_, fnt, 0);

    // Brightness row
    lv_obj_t* brRow = lv_obj_create(cont);
    lv_obj_set_width(brRow, LV_PCT(100));
    lv_obj_set_height(brRow, LV_SIZE_CONTENT);
    lv_obj_set_layout(brRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(brRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(brRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(brRow, 0, 0);
    lv_obj_set_style_bg_opa(brRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(brRow, 0, 0);
    lv_obj_t* brLbl = lv_label_create(brRow);
    lv_label_set_text(brLbl, "Bright:");
    lv_obj_set_style_text_font(brLbl, fnt, 0);
    lv_obj_set_width(brLbl, LV_SIZE_CONTENT);
    sliderBr_ = lv_slider_create(brRow);
    lv_slider_set_range(sliderBr_, 0, 255);
    lv_slider_set_value(sliderBr_, 128, LV_ANIM_OFF);
    lv_obj_set_flex_grow(sliderBr_, 1);
    lv_obj_add_event_cb(sliderBr_, onBrightnessChanged, LV_EVENT_VALUE_CHANGED, this);

    // Rotation
    lblRotation_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblRotation_, fnt, 0);

    lv_obj_t* btnRot = lv_button_create(cont);
    lv_obj_add_event_cb(btnRot, onRotateClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* lbl = lv_label_create(btnRot);
    lv_label_set_text(lbl, "Rotate 90");
    lv_obj_set_style_text_font(lbl, fnt, 0);

    // Fill buttons
    lv_obj_t* fillRow = lv_obj_create(cont);
    lv_obj_set_width(fillRow, LV_PCT(100));
    lv_obj_set_height(fillRow, LV_SIZE_CONTENT);
    lv_obj_set_layout(fillRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(fillRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fillRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fillRow, 8, 0);
    lv_obj_set_style_bg_opa(fillRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(fillRow, 0, 0);
    lv_obj_set_style_pad_all(fillRow, 0, 0);

    lv_obj_t* btnRed = lv_button_create(fillRow);
    lv_obj_add_event_cb(btnRed, onFillRedClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* lRed = lv_label_create(btnRed);
    lv_label_set_text(lRed, "Fill Red");
    lv_obj_set_style_text_font(lRed, fnt, 0);

    lv_obj_t* btnBlue = lv_button_create(fillRow);
    lv_obj_add_event_cb(btnBlue, onFillBlueClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* lBlue = lv_label_create(btnBlue);
    lv_label_set_text(lBlue, "Fill Blue");
    lv_obj_set_style_text_font(lBlue, fnt, 0);

    // Text test button
    lv_obj_t* btnText = lv_button_create(cont);
    lv_obj_add_event_cb(btnText, onWriteTextClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* lText = lv_label_create(btnText);
    lv_label_set_text(lText, "Write Text");
    lv_obj_set_style_text_font(lText, fnt, 0);

    Device* i2c = device_find_by_name("i2c1");
    if (!i2c) {
        lv_label_set_text(lblStatus_, "i2c1 not found");
        return;
    }

    // Try standalone first, then scan PaHub channels
    if (lcd_.begin(i2c)) {
        usingPaHub_ = false;
    } else if (hub_.begin(i2c)) {
        usingPaHub_ = true;
        bool found = false;
        for (uint8_t ch = 0; ch < UnitPaHub::NUM_CHANNELS && !found; ch++) {
            hub_.select(ch);
            if (lcd_.begin(i2c)) { found = true; lcdChannel_ = ch; }
        }
        if (!found) {
            hub_.deselect();
            lv_label_set_text(lblStatus_, "LCD Unit not found");
            return;
        }
    } else {
        lv_label_set_text(lblStatus_, "LCD Unit not found");
        return;
    }

    lv_label_set_text(lblStatus_, "LCD ready");
    lv_label_set_text_fmt(lblRotation_, "Rotation: %d (portrait)", (int)rotation_);
    lcd_.setBrightness(128);
    lcd_.fillScreen(0x0000);
}

void TestUnitLcd::selectIfNeeded() {
    if (usingPaHub_ && hub_.isPresent())
        hub_.select(lcdChannel_);
}

void TestUnitLcd::onStop() {
    selectIfNeeded();
    if (lcd_.isPresent()) lcd_.setBrightness(0);
    if (usingPaHub_ && hub_.isPresent()) hub_.deselect();
    lblStatus_ = sliderBr_ = lblRotation_ = nullptr;
}

void TestUnitLcd::onBrightnessChanged(lv_event_t* e) {
    auto* self = static_cast<TestUnitLcd*>(lv_event_get_user_data(e));
    self->selectIfNeeded();
    if (!self->lcd_.isPresent()) return;
    self->lcd_.setBrightness((uint8_t)lv_slider_get_value(self->sliderBr_));
}

void TestUnitLcd::onRotateClicked(lv_event_t* e) {
    auto* self = static_cast<TestUnitLcd*>(lv_event_get_user_data(e));
    self->selectIfNeeded();
    if (!self->lcd_.isPresent()) return;
    self->rotation_ = (self->rotation_ + 1) & 0x03;
    self->lcd_.setRotation(self->rotation_);
    const char* names[] = { "0 (portrait)", "1 (landscape)", "2 (portrait flip)", "3 (landscape flip)" };
    lv_label_set_text_fmt(self->lblRotation_, "Rotation: %s", names[self->rotation_]);
}

void TestUnitLcd::onFillRedClicked(lv_event_t* e) {
    auto* self = static_cast<TestUnitLcd*>(lv_event_get_user_data(e));
    self->selectIfNeeded();
    if (!self->lcd_.isPresent()) return;
    self->lcd_.fillScreen(UnitLcd::rgb888to565(0xFF0000));
}

void TestUnitLcd::onFillBlueClicked(lv_event_t* e) {
    auto* self = static_cast<TestUnitLcd*>(lv_event_get_user_data(e));
    self->selectIfNeeded();
    if (!self->lcd_.isPresent()) return;
    self->lcd_.fillScreen(UnitLcd::rgb888to565(0x0000FF));
}

void TestUnitLcd::onWriteTextClicked(lv_event_t* e) {
    auto* self = static_cast<TestUnitLcd*>(lv_event_get_user_data(e));
    self->selectIfNeeded();
    if (!self->lcd_.isPresent()) return;
    self->lcd_.fillScreen(0x0000);
    uint16_t white  = UnitLcd::rgb888to565(0xFFFFFF);
    uint16_t yellow = UnitLcd::rgb888to565(0xFFFF00);
    uint16_t cyan   = UnitLcd::rgb888to565(0x00FFFF);
    uint16_t red    = UnitLcd::rgb888to565(0xFF4444);
    uint16_t black  = 0x0000;
    uint16_t h = self->lcd_.height();
    uint16_t w = self->lcd_.width();
    // 5x7 bitmap font: 1 char = 7px tall, 6px wide; scale-1 row pitch = 9px (7+2 gap)
    static constexpr uint16_t FONT1_ROW_H  = 9;   // row height at scale 1
    static constexpr uint16_t FONT1_CHAR_W = 6;   // char advance at scale 1
    static constexpr uint16_t FONT1_COL_W  = FONT1_CHAR_W * 2 + 1; // "R" + margin
    int16_t bottomY = (h > FONT1_ROW_H)  ? (int16_t)(h - FONT1_ROW_H)  : 0;
    int16_t middleY = (int16_t)(h / 2);
    int16_t rightX  = (w > FONT1_COL_W)  ? (int16_t)(w - FONT1_COL_W)  : 0;
    // Scale-2 text: 14px tall, 12px pitch
    self->lcd_.drawText(4, 8,                "HELLO",    yellow, black, 2);
    self->lcd_.drawText(4, 32,               "WORLD",    cyan,   black, 2);
    self->lcd_.drawText(4, 4,                "TOP-LEFT", white,  black, 1);
    self->lcd_.drawText(4, bottomY,          "BOTTOM",   red,    black, 1);
    self->lcd_.drawText(4, middleY,          "MIDDLE",   white,  black, 1);
    // Right-side marker so portrait/landscape are visually distinct
    self->lcd_.drawText(rightX, 4, "R", white, black, 1);
}
