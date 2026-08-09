/**
 * @file SettingsView.h
 * @brief Settings view for TamaTac preferences
 */
#pragma once

#include <lvgl.h>

struct Context;

// Decay speed multipliers
enum class DecaySpeed {
    Slow = 0,    // 0.5x decay
    Normal = 1,  // 1x decay
    Fast = 2     // 2x decay
};

struct SettingsViewState {
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* soundSwitch = nullptr;
    lv_obj_t* decayDropdown = nullptr;
};

void settingsViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx);
void settingsViewStop(Context* ctx);

// Load/save settings
void settingsViewLoadSettings(bool* soundEnabled, DecaySpeed* decaySpeed);
void settingsViewSaveSettings(bool soundEnabled, DecaySpeed decaySpeed);
