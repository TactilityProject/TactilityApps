#include "AirMouse.h"

#include <lvgl/fonts.h>
#include <lvgl/widgets/sliderbox.h>

#include <cstdio>

namespace {

lv_obj_t* makeSettingsRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_scrollable(row, false);
    return row;
}

lv_obj_t* makeSettingsSwitch(lv_obj_t* parent, const char* title, bool checked, lv_event_cb_t callback, void* userData) {
    lv_obj_t* row = makeSettingsRow(parent);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, title);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_t* toggle = lv_switch_create(row);
    if (checked) lv_obj_add_state(toggle, LV_STATE_CHECKED);
    lv_obj_add_event_cb(toggle, callback, LV_EVENT_VALUE_CHANGED, userData);
    return toggle;
}

const char* invertLabel(int mode) {
    switch (mode & 0x03) {
        case 0x01: return "Inv:X";
        case 0x02: return "Inv:Y";
        case 0x03: return "Inv:XY";
        default:   return "Inv:off";
    }
}

// Not persisted here: a slider drag fires this for every step it snaps to. onBackClicked()
// and airMouseTeardown() save the settings file instead.
void onSensitivityChanged(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    ctx->sensitivityPercent = static_cast<int>(lvgl_sliderbox_get_value(lv_event_get_target_obj(event)));
    applySettings(ctx);
}

void onInvertClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    ctx->invertMode = (ctx->invertMode + 1) & 0x03;
    applySettings(ctx);
    if (ctx->invertBtn) {
        lv_obj_t* label = lv_obj_get_child(ctx->invertBtn, 0);
        if (label) lv_label_set_text(label, invertLabel(ctx->invertMode));
    }
    airMouseRequestSaveSettings(ctx);
}

void onCalibrateClicked(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    ctx->motion.beginCalibration();
}

void onOrientationToggled(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    ctx->useOrientation = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    applySettings(ctx);
    airMouseRequestSaveSettings(ctx);
}

void onAutoClutchToggled(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_VALUE_CHANGED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (!ctx) return;
    ctx->autoClutch = lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
    applySettings(ctx);
    airMouseRequestSaveSettings(ctx);
}

} // namespace

void settingsViewCreate(Context* ctx) {
    lv_obj_set_scroll_dir(ctx->content, LV_DIR_VER);

    // Status readout (calibration / orientation mode)
    ctx->gyroLabel = lv_label_create(ctx->content);
    lv_label_set_text(ctx->gyroLabel, "bias:--");
    lv_obj_set_width(ctx->gyroLabel, LV_PCT(100));
    lv_obj_set_style_text_font(ctx->gyroLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);

    // Sensitivity: the sliderbox carries its own value label and "-"/"+" steps, so its
    // title goes on the line above - a title and a full box don't share a narrow row.
    lv_obj_t* sensTitle = lv_label_create(ctx->content);
    lv_label_set_text(sensTitle, "Sensitivity (%)");
    ctx->sensitivityBox = lvgl_sliderbox_create(ctx->content, 25, 300, 5, ctx->sensitivityPercent);
    lvgl_sliderbox_add_value_changed_cb(ctx->sensitivityBox, onSensitivityChanged, ctx);

    // Pointer direction (cycles Inv: off/X/Y/XY)
    lv_obj_t* invRow = makeSettingsRow(ctx->content);
    lv_obj_set_flex_align(invRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* invTitle = lv_label_create(invRow);
    lv_label_set_text(invTitle, "Pointer direction");
    lv_obj_set_flex_grow(invTitle, 1);
    ctx->invertBtn = lv_btn_create(invRow);
    lv_obj_add_event_cb(ctx->invertBtn, onInvertClicked, LV_EVENT_CLICKED, ctx);
    lv_obj_t* invertLabelObj = lv_label_create(ctx->invertBtn);
    lv_label_set_text(invertLabelObj, invertLabel(ctx->invertMode));
    lv_obj_set_style_text_font(invertLabelObj, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_center(invertLabelObj);

    // Tilt-aware orientation
    ctx->orientationSwitch = makeSettingsSwitch(ctx->content, "Tilt-aware orientation (use gravity)", ctx->useOrientation, onOrientationToggled, ctx);

    // Auto-clutch: freeze the pointer while the device is being repositioned.
    // Default-on for devices with no touch screen, where the manual clutch
    // (both click pads at once) can't be pressed.
    ctx->autoClutchSwitch = makeSettingsSwitch(ctx->content, "Freeze pointer while repositioning", ctx->autoClutch, onAutoClutchToggled, ctx);

    // Re-calibrate
    ctx->calibrateBtn = lv_btn_create(ctx->content);
    lv_obj_t* calLabel = lv_label_create(ctx->calibrateBtn);
    lv_label_set_text(calLabel, "Calibrate gyro");
    lv_obj_center(calLabel);
    lv_obj_add_event_cb(ctx->calibrateBtn, onCalibrateClicked, LV_EVENT_CLICKED, ctx);

    lv_obj_t* note = lv_label_create(ctx->content);
    lv_label_set_long_mode(note, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(note, LV_PCT(100));
    lv_label_set_text(note, "Settings are saved automatically.");
    lv_obj_set_style_text_font(note, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
}
