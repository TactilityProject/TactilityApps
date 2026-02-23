/**
 * @file SettingsView.h
 * @brief Settings view for TamaTac preferences
 */
#pragma once

#include <lvgl.h>

class TamaTac;

// Decay speed multipliers
enum class DecaySpeed {
    Slow = 0,    // 0.5x decay
    Normal = 1,  // 1x decay
    Fast = 2     // 2x decay
};

class SettingsView {
private:
    TamaTac* app = nullptr;
    lv_obj_t* parent = nullptr;

    // UI elements
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* soundSwitch = nullptr;
    lv_obj_t* decayDropdown = nullptr;

public:
    SettingsView() = default;
    ~SettingsView() = default;

    SettingsView(const SettingsView&) = delete;
    SettingsView& operator=(const SettingsView&) = delete;

    void onStart(lv_obj_t* parentWidget, TamaTac* appInstance);
    void onStop();

    // Load/save settings
    static void loadSettings(bool* soundEnabled, DecaySpeed* decaySpeed);
    static void saveSettings(bool soundEnabled, DecaySpeed decaySpeed);

private:
    lv_obj_t* createSettingRow(lv_obj_t* parentContainer, const char* labelText, bool isSmall = false, bool isXLarge = false);

    // Static event handlers
    static void onSoundToggled(lv_event_t* e);
    static void onDecayChanged(lv_event_t* e);
};
