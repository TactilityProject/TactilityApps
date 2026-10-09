// SPDX-License-Identifier: GPL-3.0
#pragma once

#include <lvgl/lvgl.h>

#include <app/instance.h>

#include "TonePlayback.h"

#include <tactility/device.h>

/** Frequency presets excluding the "Sweep" option, which is appended by the UI. */
constexpr uint32_t TONE_FREQUENCY_PRESET_COUNT = 9; // 8 fixed tones + sweep
constexpr uint32_t TONE_WAVEFORM_COUNT = 5;
constexpr uint32_t TONE_CHANNEL_COUNT = 2;

struct Context {
    AppInstanceId appInstanceId = 0;

    TonePlayback playback;

    // UI-only state, touched exclusively on the LVGL thread.
    uint8_t volumePercent = 100;
    uint8_t selectedChannel = 1; // index into channelButtons; 0 = mono, 1 = stereo
    bool updatingFrequencyInput = false;

    // Widget handles; nulled in toneDestroyWidgets() once the live widgets are gone.
    lv_obj_t* playButton = nullptr;
    lv_obj_t* playButtonLabel = nullptr;
    lv_obj_t* readoutFrequencyLabel = nullptr;
    lv_obj_t* readoutStatusLabel = nullptr;
    lv_obj_t* frequencyInput = nullptr;
    lv_obj_t* presetButtons[TONE_FREQUENCY_PRESET_COUNT] = {};
    lv_obj_t* waveformButtons[TONE_WAVEFORM_COUNT] = {};
    lv_obj_t* channelButtons[TONE_CHANNEL_COUNT] = {};
    lv_obj_t* sweepCard = nullptr;
    lv_obj_t* sweepMinInput = nullptr;
    lv_obj_t* sweepMaxInput = nullptr;
    lv_obj_t* sweepPeriodInput = nullptr;
    lv_obj_t* volumeBox = nullptr;
    lv_timer_t* refreshTimer = nullptr;
};

void toneCreateWidgets(lv_obj_t* root, void* userData);
void toneDestroyWidgets(void* userData);