#pragma once

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>
#include <tactility/device.h>
#include <tactility/drivers/bluetooth.h>
#include <tactility/drivers/bluetooth_hid_device.h>
#include <tactility/drivers/keyboard.h>
#include <atomic>

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;
    struct TaskEventGroup* eventGroup = nullptr; // borrowed from main(); outlives ctx

    // UI elements
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* switchWidget = nullptr;
    lv_obj_t* buttonMatrix = nullptr;
    lv_timer_t* keyHighlightTimer = nullptr;
    uint32_t activeKeyBtn = LV_BTNMATRIX_BTN_NONE;
    bool keyboardActive = false; // true when matrix has focus group + editing mode

    // HAL device handles
    struct Device* btDevice = nullptr;
    struct Device* hidDevice = nullptr;
    struct BtEventSubscription btEventSub {};

    // State - accessed from both LVGL thread and the app's own task (BT event poll loop)
    std::atomic<bool> isEnabled       {false};
    std::atomic<bool> radioEnabling   {false}; // true while waiting for radio to come ON
    std::atomic<bool> radioWasOff     {false}; // true if we turned the radio on (restore on exit)
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void mediaKeysCreateWidgets(lv_obj_t* parent, void* userData);

/** Looks up the BT device and subscribes to its events, claiming a bit in ctx->eventGroup.
 *  Must be called once, before the app's main loop starts blocking on that event group (its bit
 *  has to already be claimed by the first task_event_group_wait_any() call - see that function's
 *  warning about bits claimed mid-wait). @return true on success. */
bool mediaKeysInitBt(Context* ctx);

/** Drains any BT events queued for ctx and reacts to them (radio state, HID profile state).
 *  Call from the app's main loop after task_event_group_wait_any() returns. */
void mediaKeysProcessBtEvents(Context* ctx);

/** Removes the BT event subscription, stops HID, restores radio state, releases widget-tracking
 *  state. Call once, after the window has been torn down. */
void mediaKeysTeardown(Context* ctx);
