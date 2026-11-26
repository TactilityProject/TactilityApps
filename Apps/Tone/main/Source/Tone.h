// SPDX-License-Identifier: GPL-3.0
#pragma once

#include <lvgl/lvgl.h>

#include <app/instance.h>

#include "TonePlayback.h"

#include <tactility/concurrent/task_event_group.h>
#include <tactility/device.h>
#include <tactility/freertos/semphr.h>

#include <atomic>

constexpr uint32_t TONE_PRESET_COUNT = 8;
constexpr uint32_t TONE_WAVEFORM_COUNT = 5;
constexpr uint32_t TONE_PREVIEW_POINTS = 101;

/** What a sweep row sets */
enum class SweepValue : uint8_t { From, To, Period };

struct Context {
    AppInstanceId appInstanceId = 0;

    TonePlayback playback;

    // Serializes UI playback control against app teardown: once `closing` is set under
    // this gate, onPlayPressed() backs out so it cannot start a task on a context that
    // main() is about to destroy.
    SemaphoreHandle_t playbackGate = nullptr;
    bool closing = false;

    // Menu and dialog actions are carried out on the app task, which actionBit wakes
    TaskEventGroup* eventGroup = nullptr;
    uint32_t actionBit = 0;
    std::atomic<int32_t> action { 0 };
    std::atomic<int32_t> actionValue { 0 };

    // UI-only state, guarded by the LVGL lock
    uint8_t volumePercent = 100;
    bool sweepPage = false;   // the sweep settings are shown instead of the main page (small screens)
    bool compact = false;     // the sweep settings are on their own page
    bool presetsShown = true; // false when there is no room for them
    SweepValue editedSweepValue = SweepValue::From;
    uint8_t menuActions[3] = {};

    // Widget handles; nulled when the widgets are destroyed
    lv_obj_t* root = nullptr;
    lv_obj_t* frequencyLabel = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* frequencySlider = nullptr;
    lv_obj_t* playButton = nullptr;
    lv_obj_t* playLabel = nullptr;
    lv_obj_t* waveformChips[TONE_WAVEFORM_COUNT] = {};
    lv_obj_t* presetChips[TONE_PRESET_COUNT] = {};
    lv_obj_t* fixedChip = nullptr;
    lv_obj_t* sweepChip = nullptr;
    lv_obj_t* sweepSliders[3] = {};
    lv_obj_t* sweepLabels[3] = {};
    lv_obj_t* previewLine = nullptr;
    lv_point_precise_t previewPoints[TONE_PREVIEW_POINTS] = {};
    lv_timer_t* refreshTimer = nullptr;
};

void toneCreateWidgets(lv_obj_t* root, void* userData);
void toneDestroyWidgets(void* userData);

/** Carries out the pending menu or dialog action. Call on the app task, without the LVGL lock. */
void toneRunAction(Context* ctx);
