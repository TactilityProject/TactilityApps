/**
 * @file SettingsView.cpp
 * @brief Settings view implementation
 */

#include "SettingsView.h"
#include "Layout.h"
#include "TamaTac.h"
#include <app/paths.h>
#include <tactility/preferences.h>
#include <string>
#include <lvgl/widgets/chip.h>

namespace {

bool getPreferencesPath(std::string& outPath) {
    char path[128];
    if (app_paths_get_user_data_path("tactility.tamatac", "tamatac.properties", path, sizeof(path)) != ERROR_NONE) {
        return false;
    }
    outPath = std::string(path);
    return true;
}

lv_obj_t* createSettingRow(lv_obj_t* parentContainer, const char* labelText, bool isSmall, bool isXLarge) {
    lv_obj_t* row = lv_obj_create(parentContainer);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    int padAll = isSmall ? 4 : (isXLarge ? 16 : 8);
    int padHoriz = isSmall ? 6 : (isXLarge ? 24 : 12);
    lv_obj_set_style_pad_all(row, padAll, 0);
    lv_obj_set_style_pad_left(row, padHoriz, 0);
    lv_obj_set_style_pad_right(row, padHoriz, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a4e), 0);
    lv_obj_set_style_radius(row, isSmall ? 4 : (isXLarge ? 10 : 6), 0);

    lv_obj_t* label = lv_label_create(row);
    lv_label_set_text(label, labelText);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFFFFF), 0);

    return row;
}

void onSoundToggled(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    SettingsViewState* state = &ctx->settingsView;
    if (state->soundSwitch == nullptr) return;

    bool isChecked = lv_obj_has_state(state->soundSwitch, LV_STATE_CHECKED);

    tamaTacSetSoundEnabled(ctx, isChecked);

    // Decay speed lives in Context (single source of truth)
    settingsViewSaveSettings(isChecked, ctx->decaySpeed);
}

void syncDecayChips(SettingsViewState* state, DecaySpeed selected) {
    for (int i = 0; i < 3; i++) {
        if (state->decayChips[i] != nullptr) {
            lv_obj_set_state(state->decayChips[i], LV_STATE_CHECKED, i == static_cast<int>(selected));
        }
    }
}

void onDecayChipPressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    SettingsViewState* state = &ctx->settingsView;
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(e));

    for (int i = 0; i < 3; i++) {
        if (state->decayChips[i] == chip) {
            DecaySpeed newSpeed = static_cast<DecaySpeed>(i);
            tamaTacSetDecaySpeed(ctx, newSpeed);
            syncDecayChips(state, newSpeed);

            bool soundEnabled = state->soundSwitch != nullptr &&
                lv_obj_has_state(state->soundSwitch, LV_STATE_CHECKED);
            settingsViewSaveSettings(soundEnabled, newSpeed);
            break;
        }
    }
}

} // namespace

constexpr const char* DECAY_NAMES[] = { "Slow", "Normal", "Fast" };

void settingsViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx) {
    SettingsViewState* state = &ctx->settingsView;

    // Shared viewport detection (display resolution + orientation)
    TamaViewport vp = tamaGetViewport();
    bool isSmall = tamaIsSmall(vp);
    bool isXLarge = tamaIsXLarge(vp);

    // Load current settings
    bool soundEnabled = true;
    DecaySpeed decaySpeed = DecaySpeed::Normal;
    settingsViewLoadSettings(&soundEnabled, &decaySpeed);

    // Scaled dimensions
    int padAll = isSmall ? 4 : (isXLarge ? 16 : 8);
    int padRowVal = isSmall ? 4 : (isXLarge ? 16 : 8);

    // Main wrapper
    state->mainWrapper = lv_obj_create(parentWidget);
    lv_obj_set_size(state->mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(state->mainWrapper, padAll, 0);
    lv_obj_set_style_pad_row(state->mainWrapper, padRowVal, 0);
    lv_obj_set_style_bg_opa(state->mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->mainWrapper, 0, 0);
    lv_obj_set_flex_flow(state->mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state->mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Vertical scroll safety: rows + chips can overflow short screens
    lv_obj_set_scrollable(state->mainWrapper, true);
    lv_obj_set_scroll_dir(state->mainWrapper, LV_DIR_VER);

    // Sound setting row
    lv_obj_t* soundRow = createSettingRow(state->mainWrapper, "Sound", isSmall, isXLarge);

    state->soundSwitch = lv_switch_create(soundRow);
    lv_obj_set_style_bg_color(state->soundSwitch, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(state->soundSwitch, lv_color_hex(0x00AA00), LV_PART_INDICATOR);
    if (soundEnabled) {
        lv_obj_add_state(state->soundSwitch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(state->soundSwitch, onSoundToggled, LV_EVENT_VALUE_CHANGED, ctx);

    // Decay speed row (label + 3 exclusive chips, wraps on narrow portrait screens)
    lv_obj_t* decayRow = createSettingRow(state->mainWrapper, "Speed", isSmall, isXLarge);
    lv_obj_set_flex_flow(decayRow, LV_FLEX_FLOW_ROW_WRAP);

    lv_obj_t* chips = lv_obj_create(decayRow);
    lv_obj_set_size(chips, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_bg_opa(chips, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(chips, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(chips, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(chips, isSmall ? 2 : 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(chips, isSmall ? 2 : 4, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(chips, false);

    for (int32_t i = 0; i < 3; i++) {
        auto* chip = lvgl_chip_create(chips);
        lv_obj_t* chipLabel = lv_label_create(chip);
        lv_label_set_text(chipLabel, DECAY_NAMES[i]);
        lv_obj_center(chipLabel);
        lv_obj_add_event_cb(chip, onDecayChipPressed, LV_EVENT_SHORT_CLICKED, ctx);
        state->decayChips[i] = chip;
    }
    syncDecayChips(state, decaySpeed);
}

void settingsViewStop(Context* ctx) {
    SettingsViewState* state = &ctx->settingsView;
    state->mainWrapper = nullptr;
    state->soundSwitch = nullptr;
    for (int i = 0; i < 3; i++) {
        state->decayChips[i] = nullptr;
    }
}

void settingsViewLoadSettings(bool* soundEnabled, DecaySpeed* decaySpeed) {
    *soundEnabled = true;
    int32_t speed = static_cast<int32_t>(DecaySpeed::Normal);

    std::string path;
    if (getPreferencesPath(path)) {
        if (Preferences* prefs = preferences_open(path.c_str())) {
            preferences_opt_bool(prefs, "soundOn", soundEnabled);
            preferences_opt_int32(prefs, "decaySpd", &speed);
            preferences_close(prefs);
        }
    }

    if (speed < 0 || speed > 2) {
        speed = static_cast<int32_t>(DecaySpeed::Normal);
    }
    *decaySpeed = static_cast<DecaySpeed>(speed);
}

void settingsViewSaveSettings(bool soundEnabled, DecaySpeed decaySpeed) {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;
    preferences_put_bool(prefs, "soundOn", soundEnabled);
    preferences_put_int32(prefs, "decaySpd", static_cast<int32_t>(decaySpeed));
    preferences_close(prefs);
}
