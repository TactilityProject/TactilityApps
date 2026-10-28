#pragma once

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>
#include <tactility/device.h>
#include <tactility/drivers/bluetooth.h>
#include <tactility/drivers/bluetooth_hid_device.h>
#include <tactility/drivers/imu.h>
#include "Motion.h"
#include <atomic>
#include <cstdint>

// Which page of the single window is showing.
enum class AirMouseView {
    Main,
    Settings,
};

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;
    struct TaskEventGroup* eventGroup = nullptr; // borrowed from main(); outlives ctx

    // UI elements
    lv_obj_t* root            = nullptr; // the window's content parent, rebuilt on view change
    lv_obj_t* toolbar         = nullptr;
    lv_obj_t* content         = nullptr; // below the toolbar; the visible view fills this
    AirMouseView view         = AirMouseView::Main;
    lv_obj_t* switchWidget    = nullptr;
    lv_obj_t* gyroLabel       = nullptr;
    lv_obj_t* leftClickBtn    = nullptr;
    lv_obj_t* rightClickBtn   = nullptr;
    lv_timer_t* motionTimer   = nullptr;
    lv_obj_t* sensitivityBox  = nullptr; // sliderbox: slider + "-"/"+" steps + its own value label
    lv_obj_t* invertBtn         = nullptr;
    lv_obj_t* calibrateBtn      = nullptr;
    lv_obj_t* orientationSwitch = nullptr;
    lv_obj_t* autoClutchSwitch  = nullptr;
    lv_timer_t* viewTimer     = nullptr; // one-shot: deferred view rebuild

    // IMU device (device tree, persistent across widget rebuilds; released in airMouseTeardown)
    Device* imuDevice   = nullptr;
    // External I2C fallback (used when no device-tree entry exists - a bare/Unit-style MPU6886
    // plugged into an exposed I2C bus, any device can use it)
    Device* extI2cCtrl  = nullptr;
    uint8_t extImuAddr  = 0;
    // Sensor -> device frame transform for the external module (the devicetree drivers apply
    // their own mount-matrix; a bare chip has none, so we keep one here). Persisted.
    airmouse::MountMatrix extMountMatrix {};

    // Motion pipeline (bias calibration, orientation mapping, filtering, sub-pixel carry)
    airmouse::MotionProcessor motion {};
    uint32_t lastMotionTick = 0;

    // Motion settings (persisted)
    int  sensitivityPercent = 100; // 25..300
    int  invertMode         = 0;   // bit 0 = invert X, bit 1 = invert Y
    bool useOrientation     = true;
    bool autoClutch         = false; // freeze the pointer while repositioning the device

    // HAL device handles
    struct Device* btDevice = nullptr;
    struct Device* hidDevice = nullptr;
    struct BtEventSubscription btEventSub {};

    // State - accessed from both LVGL thread and the app's own task (BT event poll loop)
    std::atomic<bool> isEnabled     {false};
    std::atomic<bool> radioEnabling {false}; // true while waiting for radio to come ON
    std::atomic<bool> radioWasOff   {false}; // true if we turned the radio on (restore on exit)

    // Work that can touch flash must not run on the LVGL task: its stack is in PSRAM and flash
    // access disables the cache. The BT toggle runs on the app's own task (internal RAM, already
    // polling BT events); settings are written by the kernel's file_ops task.
    uint32_t requestBit = 0;             // claimed in main() before the first wait
    std::atomic<int> pendingToggle {-1}; // -1 = none, 0 = off, 1 = on

    // Mouse HID state
    uint8_t buttons = 0; // bit 0 = left, bit 1 = right
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void airMouseCreateWidgets(lv_obj_t* parent, void* userData);

/** Looks up the IMU and BT devices and subscribes to BT events, claiming a bit in ctx->eventGroup.
 *  Must be called once, before the app's main loop starts blocking on that event group (its bit
 *  has to already be claimed by the first task_event_group_wait_any() call - see that function's
 *  warning about bits claimed mid-wait). @return true if a BT device was found (IMU absence is
 *  reported separately, in the UI, since the app can still show useful state without one). */
bool airMouseInitBt(Context* ctx);

/** Drains any BT events queued for ctx and reacts to them (radio state, HID profile state).
 *  Call from the app's main loop after task_event_group_wait_any() returns. */
void airMouseProcessBtEvents(Context* ctx);

/** Asks the app task to turn the pointer on or off. Call from an LVGL event handler. */
void airMouseRequestToggle(Context* ctx, bool enabled);

/** Queues a settings save on the kernel file_ops task. Call from an LVGL event handler. */
void airMouseRequestSaveSettings(Context* ctx);

/** Runs the requests queued by the two functions above. Call from the app's main loop. */
void airMouseProcessRequests(Context* ctx);

/** Removes the BT event subscription, stops HID, restores radio state, releases the IMU device
 *  reference, releases widget-tracking state. Call once, after the window has been torn down. */
void airMouseTeardown(Context* ctx);

// ── Views ─────────────────────────────────────────────────────────────────────
// buildView() owns the toolbar and the content container (both views need them in that
// order); each view file only fills ctx->content - see MainView.cpp and SettingsView.cpp.

/** Rebuilds the window's widgets (toolbar + content) for ctx->view. */
void buildView(Context* ctx);

/** Shows another view. The switch happens on the next LVGL tick, because the caller is
 *  usually inside an event of a widget that the rebuild deletes. */
void showView(Context* ctx, AirMouseView view);

/** Fills ctx->content with the main view: the large left/right click pads. */
void mainViewCreate(Context* ctx);

/** Fills ctx->content with the settings view: status, sensitivity, direction, switches, calibrate. */
void settingsViewCreate(Context* ctx);

// ── Shared state used by more than one view ───────────────────────────────────

/** Pushes ctx's persisted motion settings into the pipeline. */
void applySettings(Context* ctx);

/** Turns the pointer on or off: the toolbar switch, and the BT radio going away. */
void handleSwitchToggle(Context* ctx, bool enabled);

/** Sends a mouse report with ctx->buttons and the given movement. */
void sendMouseReport(Context* ctx, int8_t dx, int8_t dy);
