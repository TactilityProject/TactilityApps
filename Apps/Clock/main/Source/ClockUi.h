// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ClockSettings.h"

#include <app/instance.h>
#include <tactility/concurrent/task_event_group.h>
#include <lvgl.h>

namespace clockapp {

struct Context {
    AppInstanceId appInstanceId = 0;
    bool analog = false;
    bool fullscreen = false;
    bool format24h = true;
    DateFormat dateFormat = DateFormat::MonthDayYear;

    lv_obj_t* toolbar = nullptr;
    lv_obj_t* content = nullptr;
    lv_obj_t* menuButton = nullptr;
    /** Covers the clock while it's fullscreen, to catch the tap that ends it */
    lv_obj_t* exitFullscreen = nullptr;

    // The menu's choice is handled on the app task, which menuBit wakes
    TaskEventGroup* eventGroup = nullptr;
    uint32_t menuBit = 0;
    int32_t menuChoice = -1;

    lv_obj_t* timeLabel = nullptr;
    lv_obj_t* secondsLabel = nullptr;
    lv_obj_t* meridiemLabel = nullptr;
    lv_obj_t* weekdayLabel = nullptr;
    lv_obj_t* dateLabel = nullptr;

    lv_obj_t* invalidIcon = nullptr;
    lv_obj_t* invalidBody = nullptr;

    lv_obj_t* analogFace = nullptr;
    lv_obj_t* hourHand = nullptr;
    lv_obj_t* minuteHand = nullptr;
    lv_obj_t* secondHand = nullptr;
    lv_obj_t* centerCap = nullptr;
    lv_obj_t* tickLines[12] = {};
    lv_point_precise_t tickPoints[12][2] = {};
    lv_point_precise_t hourPoints[2] = {};
    lv_point_precise_t minutePoints[2] = {};
    lv_point_precise_t secondPoints[2] = {};

    lv_timer_t* timer = nullptr;
    bool timeValid = false;
    bool contentBuilt = false;
};

void buildUi(Context* context, lv_obj_t* parent);
void destroyUi(Context* context);
void refreshClock(Context* context);
/** Applies the menu choice. Call on the app task, without the LVGL lock. */
void applyMenuChoice(Context* context);

}
