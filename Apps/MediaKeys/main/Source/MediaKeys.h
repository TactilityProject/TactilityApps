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

    // State - accessed from both LVGL thread and BT callback thread
    std::atomic<bool> isEnabled       {false};
    std::atomic<bool> radioEnabling   {false}; // true while waiting for radio to come ON
    std::atomic<bool> radioWasOff     {false}; // true if we turned the radio on (restore on exit)
    std::atomic<bool> deviceWasStarted{false}; // true if we called device_start (restore on exit)
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void mediaKeysCreateWidgets(lv_obj_t* parent, void* userData);

/** Removes the BT callback, stops HID, restores radio/device state, releases widget-tracking
 *  state. Call once, after the window has been torn down. */
void mediaKeysTeardown(Context* ctx);
