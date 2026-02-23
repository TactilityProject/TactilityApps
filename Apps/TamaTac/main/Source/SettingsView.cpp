/**
 * @file SettingsView.cpp
 * @brief Settings view implementation
 */

#include "SettingsView.h"
#include "TamaTac.h"
#include <TactilityCpp/Preferences.h>

lv_obj_t* SettingsView::createSettingRow(lv_obj_t* parentContainer, const char* labelText, bool isSmall, bool isXLarge) {
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

void SettingsView::onStart(lv_obj_t* parentWidget, TamaTac* appInstance) {
    parent = parentWidget;
    app = appInstance;

    // Detect screen size for responsive layout
    // Use display resolution for reliable sizing (parent may not be laid out yet on first load)
    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    // Load current settings
    bool soundEnabled = true;
    DecaySpeed decaySpeed = DecaySpeed::Normal;
    loadSettings(&soundEnabled, &decaySpeed);

    // Scaled dimensions
    int padAll = isSmall ? 4 : (isXLarge ? 16 : 8);
    int padRowVal = isSmall ? 4 : (isXLarge ? 16 : 8);

    // Main wrapper
    mainWrapper = lv_obj_create(parent);
    lv_obj_set_size(mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(mainWrapper, padAll, 0);
    lv_obj_set_style_pad_row(mainWrapper, padRowVal, 0);
    lv_obj_set_style_bg_opa(mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mainWrapper, 0, 0);
    lv_obj_set_flex_flow(mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Sound setting row
    lv_obj_t* soundRow = createSettingRow(mainWrapper, "Sound", isSmall, isXLarge);

    soundSwitch = lv_switch_create(soundRow);
    lv_obj_set_style_bg_color(soundSwitch, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(soundSwitch, lv_color_hex(0x00AA00), LV_PART_INDICATOR);
    if (soundEnabled) {
        lv_obj_add_state(soundSwitch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(soundSwitch, onSoundToggled, LV_EVENT_VALUE_CHANGED, this);

    // Decay speed row
    lv_obj_t* decayRow = createSettingRow(mainWrapper, "Speed", isSmall, isXLarge);

    decayDropdown = lv_dropdown_create(decayRow);
    lv_dropdown_set_options(decayDropdown, "Slow\nNormal\nFast");
    lv_dropdown_set_selected(decayDropdown, static_cast<uint16_t>(decaySpeed));
    lv_obj_set_width(decayDropdown, isSmall ? 80 : (isXLarge ? 140 : 100));
    lv_obj_set_style_bg_color(decayDropdown, lv_color_hex(0x1a1a2e), LV_PART_MAIN);
    lv_obj_set_style_text_color(decayDropdown, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_add_event_cb(decayDropdown, onDecayChanged, LV_EVENT_VALUE_CHANGED, this);
}

void SettingsView::onStop() {
    mainWrapper = nullptr;
    soundSwitch = nullptr;
    decayDropdown = nullptr;
    parent = nullptr;
    app = nullptr;
}

void SettingsView::loadSettings(bool* soundEnabled, DecaySpeed* decaySpeed) {
    Preferences prefs("TamaTacSet");

    *soundEnabled = prefs.getBool("soundOn", true);

    int32_t speed = prefs.getInt32("decaySpd", static_cast<int32_t>(DecaySpeed::Normal));
    if (speed < 0 || speed > 2) {
        speed = static_cast<int32_t>(DecaySpeed::Normal);
    }
    *decaySpeed = static_cast<DecaySpeed>(speed);
}

void SettingsView::saveSettings(bool soundEnabled, DecaySpeed decaySpeed) {
    Preferences prefs("TamaTacSet");
    prefs.putBool("soundOn", soundEnabled);
    prefs.putInt32("decaySpd", static_cast<int32_t>(decaySpeed));
}

void SettingsView::onSoundToggled(lv_event_t* e) {
    SettingsView* view = static_cast<SettingsView*>(lv_event_get_user_data(e));
    if (view == nullptr || view->soundSwitch == nullptr || view->decayDropdown == nullptr || view->app == nullptr) return;

    bool isChecked = lv_obj_has_state(view->soundSwitch, LV_STATE_CHECKED);

    view->app->setSoundEnabled(isChecked);

    // Read decay speed from UI widget instead of redundant preferences load
    DecaySpeed decaySpeed = static_cast<DecaySpeed>(lv_dropdown_get_selected(view->decayDropdown));
    view->saveSettings(isChecked, decaySpeed);
}

void SettingsView::onDecayChanged(lv_event_t* e) {
    SettingsView* view = static_cast<SettingsView*>(lv_event_get_user_data(e));
    if (view == nullptr || view->decayDropdown == nullptr || view->soundSwitch == nullptr || view->app == nullptr) return;

    uint16_t selected = lv_dropdown_get_selected(view->decayDropdown);

    if (selected > 2) {
        selected = 1;
    }
    DecaySpeed newSpeed = static_cast<DecaySpeed>(selected);

    view->app->setDecaySpeed(newSpeed);

    // Read sound state from UI widget instead of redundant preferences load
    bool soundEnabled = lv_obj_has_state(view->soundSwitch, LV_STATE_CHECKED);
    view->saveSettings(soundEnabled, newSpeed);
}
