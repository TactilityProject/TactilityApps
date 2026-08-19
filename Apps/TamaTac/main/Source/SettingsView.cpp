/**
 * @file SettingsView.cpp
 * @brief Settings view implementation
 */

#include "SettingsView.h"
#include "TamaTac.h"
#include <app/paths.h>
#include <tactility/preferences.h>
#include <string>

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
    if (state->soundSwitch == nullptr || state->decayDropdown == nullptr) return;

    bool isChecked = lv_obj_has_state(state->soundSwitch, LV_STATE_CHECKED);

    tamaTacSetSoundEnabled(ctx, isChecked);

    // Read decay speed from UI widget instead of redundant preferences load
    DecaySpeed decaySpeed = static_cast<DecaySpeed>(lv_dropdown_get_selected(state->decayDropdown));
    settingsViewSaveSettings(isChecked, decaySpeed);
}

void onDecayChanged(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    SettingsViewState* state = &ctx->settingsView;
    if (state->decayDropdown == nullptr || state->soundSwitch == nullptr) return;

    uint16_t selected = lv_dropdown_get_selected(state->decayDropdown);

    if (selected > 2) {
        selected = 1;
    }
    DecaySpeed newSpeed = static_cast<DecaySpeed>(selected);

    tamaTacSetDecaySpeed(ctx, newSpeed);

    // Read sound state from UI widget instead of redundant preferences load
    bool soundEnabled = lv_obj_has_state(state->soundSwitch, LV_STATE_CHECKED);
    settingsViewSaveSettings(soundEnabled, newSpeed);
}

} // namespace

void settingsViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx) {
    SettingsViewState* state = &ctx->settingsView;

    // Detect screen size for responsive layout
    // Use display resolution for reliable sizing (parent may not be laid out yet on first load)
    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

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
    lv_obj_remove_flag(state->mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Sound setting row
    lv_obj_t* soundRow = createSettingRow(state->mainWrapper, "Sound", isSmall, isXLarge);

    state->soundSwitch = lv_switch_create(soundRow);
    lv_obj_set_style_bg_color(state->soundSwitch, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(state->soundSwitch, lv_color_hex(0x00AA00), LV_PART_INDICATOR);
    if (soundEnabled) {
        lv_obj_add_state(state->soundSwitch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(state->soundSwitch, onSoundToggled, LV_EVENT_VALUE_CHANGED, ctx);

    // Decay speed row
    lv_obj_t* decayRow = createSettingRow(state->mainWrapper, "Speed", isSmall, isXLarge);

    state->decayDropdown = lv_dropdown_create(decayRow);
    lv_dropdown_set_options(state->decayDropdown, "Slow\nNormal\nFast");
    lv_dropdown_set_selected(state->decayDropdown, static_cast<uint16_t>(decaySpeed));
    lv_obj_set_width(state->decayDropdown, isSmall ? 80 : (isXLarge ? 140 : 100));
    lv_obj_set_style_bg_color(state->decayDropdown, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_text_color(state->decayDropdown, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_add_event_cb(state->decayDropdown, onDecayChanged, LV_EVENT_VALUE_CHANGED, ctx);
}

void settingsViewStop(Context* ctx) {
    SettingsViewState* state = &ctx->settingsView;
    state->mainWrapper = nullptr;
    state->soundSwitch = nullptr;
    state->decayDropdown = nullptr;
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
