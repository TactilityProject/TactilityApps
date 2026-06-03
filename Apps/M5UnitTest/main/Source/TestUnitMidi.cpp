#include "TestUnitMidi.h"
#include "UiScale.h"
#include <tactility/device.h>
#include <tactility/lvgl_fonts.h>

void TestUnitMidi::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;

    createToolbar(parent, handle, "MIDI / Synth");
    createBanner(parent, "MIDI", "UART", COLOR_UART);

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, uiPad(), 0);
    lv_obj_set_style_pad_row(cont, uiRowGap(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);

    const lv_font_t* fnt  = lvgl_get_text_font(uiFont());
    const lv_font_t* fntS = lvgl_get_text_font(FONT_SIZE_SMALL);

    lblStatus_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblStatus_, fnt, 0);

    lblChannel_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblChannel_, fntS, 0);

    lblProgram_ = lv_label_create(cont);
    lv_obj_set_style_text_font(lblProgram_, fntS, 0);

    // Channel row
    auto makeAdjRow = [&](const char* name, lv_event_cb_t down, lv_event_cb_t up) {
        lv_obj_t* row = lv_obj_create(cont);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_layout(row, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, 8, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);

        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, name);
        lv_obj_set_style_text_font(lbl, fntS, 0);
        lv_obj_set_width(lbl, LV_SIZE_CONTENT);

        lv_obj_t* bDown = lv_button_create(row);
        lv_obj_add_event_cb(bDown, down, LV_EVENT_CLICKED, this);
        lv_obj_t* lD = lv_label_create(bDown); lv_label_set_text(lD, "-");
        lv_obj_set_style_text_font(lD, fntS, 0);

        lv_obj_t* bUp = lv_button_create(row);
        lv_obj_add_event_cb(bUp, up, LV_EVENT_CLICKED, this);
        lv_obj_t* lU = lv_label_create(bUp); lv_label_set_text(lU, "+");
        lv_obj_set_style_text_font(lU, fntS, 0);
    };
    makeAdjRow("Channel:", onChDown, onChUp);
    makeAdjRow("Program:", onProgDown, onProgUp);

    // Note on/off
    lv_obj_t* noteRow = lv_obj_create(cont);
    lv_obj_set_width(noteRow, LV_PCT(100));
    lv_obj_set_height(noteRow, LV_SIZE_CONTENT);
    lv_obj_set_layout(noteRow, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(noteRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(noteRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(noteRow, 0, 0);
    lv_obj_set_style_pad_column(noteRow, 8, 0);
    lv_obj_set_style_bg_opa(noteRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(noteRow, 0, 0);

    lv_obj_t* btnOn = lv_button_create(noteRow);
    lv_obj_add_event_cb(btnOn, onNoteOnClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* lOn = lv_label_create(btnOn); lv_label_set_text(lOn, "Note On (C4)");
    lv_obj_set_style_text_font(lOn, fntS, 0);

    lv_obj_t* btnOff = lv_button_create(noteRow);
    lv_obj_add_event_cb(btnOff, onNoteOffClicked, LV_EVENT_CLICKED, this);
    lv_obj_t* lOff = lv_label_create(btnOff); lv_label_set_text(lOff, "Note Off");
    lv_obj_set_style_text_font(lOff, fntS, 0);

    Device* uart = device_find_by_name(UART_DEVICE);
    if (!uart || !device_is_ready(uart) || !unit_.begin(uart)) {
        lv_label_set_text(lblStatus_, "MIDI UART not available");
        updateLabels();
        return;
    }

    lv_label_set_text(lblStatus_, "MIDI ready (31250 bps)");
    unit_.programChange(channel_, program_);
    updateLabels();
}

void TestUnitMidi::onStop() {
    if (notePlaying_ && unit_.isPresent()) {
        unit_.noteOff(channel_, note_);
        notePlaying_ = false;
    }
    unit_.end();
    lblStatus_ = lblChannel_ = lblProgram_ = nullptr;
}

void TestUnitMidi::updateLabels() {
    lv_label_set_text_fmt(lblChannel_, "Channel: %d", (int)channel_ + 1);
    lv_label_set_text_fmt(lblProgram_, "Program: %d", (int)program_ + 1);
}

void TestUnitMidi::onNoteOnClicked(lv_event_t* e) {
    auto* self = static_cast<TestUnitMidi*>(lv_event_get_user_data(e));
    if (!self->unit_.isPresent()) return;
    self->unit_.noteOn(self->channel_, self->note_, 100);
    self->notePlaying_ = true;
}

void TestUnitMidi::onNoteOffClicked(lv_event_t* e) {
    auto* self = static_cast<TestUnitMidi*>(lv_event_get_user_data(e));
    if (!self->unit_.isPresent()) return;
    self->unit_.noteOff(self->channel_, self->note_);
    self->notePlaying_ = false;
}

void TestUnitMidi::onChDown(lv_event_t* e) {
    auto* self = static_cast<TestUnitMidi*>(lv_event_get_user_data(e));
    if (self->channel_ > 0) {
        if (self->notePlaying_ && self->unit_.isPresent()) {
            self->unit_.noteOff(self->channel_, self->note_);
            self->notePlaying_ = false;
        }
        self->channel_--;
        self->updateLabels();
    }
}

void TestUnitMidi::onChUp(lv_event_t* e) {
    auto* self = static_cast<TestUnitMidi*>(lv_event_get_user_data(e));
    if (self->channel_ < 15) {
        if (self->notePlaying_ && self->unit_.isPresent()) {
            self->unit_.noteOff(self->channel_, self->note_);
            self->notePlaying_ = false;
        }
        self->channel_++;
        self->updateLabels();
    }
}

void TestUnitMidi::onProgDown(lv_event_t* e) {
    auto* self = static_cast<TestUnitMidi*>(lv_event_get_user_data(e));
    if (self->program_ > 0) {
        self->program_--;
        if (self->unit_.isPresent()) self->unit_.programChange(self->channel_, self->program_);
        self->updateLabels();
    }
}

void TestUnitMidi::onProgUp(lv_event_t* e) {
    auto* self = static_cast<TestUnitMidi*>(lv_event_get_user_data(e));
    if (self->program_ < 127) {
        self->program_++;
        if (self->unit_.isPresent()) self->unit_.programChange(self->channel_, self->program_);
        self->updateLabels();
    }
}
