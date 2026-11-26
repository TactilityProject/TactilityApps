#include "Application.h"
#include "DoomMain.h"

#include <tactility/log.h>
#include <lvgl_window_manager/dialog.h>
#include <lvgl_window_manager/window_manager.h>

#include <app/event.h>
#include <app/scheduler.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/touch.h>
#include <lvgl/lvgl.h>
#include <lvgl/module.h>
#include <tactility/module.h>

constexpr auto* TAG = "Doom";

// Shows an error dialog and waits for it to close. A dialog needs a window, so an empty one is created for it.
static void showErrorAndWait(AppInstanceId appInstanceId, const char* message) {
    struct DialogState {
        TaskEventGroup eventGroup {};
        uint32_t closedBit = 0;
    } state;
    task_event_group_construct(&state.eventGroup);
    check(task_event_group_claim_bit(&state.eventGroup, &state.closedBit) == ERROR_NONE);
    AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &state.eventGroup) == ERROR_NONE);
    const WindowId window = window_manager_create(appInstanceId, [](lv_obj_t*, void*) {}, nullptr);

    static const char* const buttons[] = { "OK" };
    const error_t result = window_manager_alert_dialog(appInstanceId, "Error", message, buttons, 1, [](int32_t, void* userData) {
        auto* dialogState = static_cast<DialogState*>(userData);
        task_event_group_signal(&dialogState->eventGroup, dialogState->closedBit);
    }, &state);

    bool done = result != ERROR_NONE;
    while (!done) {
        uint32_t flags = 0;
        task_event_group_wait_any(&state.eventGroup, &flags, portMAX_DELAY);
        done = (flags & state.closedBit) != 0;
        AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                done = true;
            }
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&state.eventGroup, state.closedBit);
    task_event_group_destruct(&state.eventGroup);
}

// Reads the user's view of the display from LVGL, before it's stopped.
//
// LVGL rotates either in hardware (displays with swap_xy and mirror: the panel itself is
// reconfigured, so drawing happens in the user's orientation) or in software (e.g. MIPI-DSI and
// RGB panels, which stay in their native orientation). Either way the touch driver reports native
// coordinates, which LVGL rotates itself.
static DoomScreen getScreen(Device* display) {
    const uint32_t nativeW = display_get_resolution_x(display);
    const uint32_t nativeH = display_get_resolution_y(display);
    lv_display_t* lvglDisplay = lv_display_get_default();
    const int rotation = lvglDisplay != nullptr ? (int)lv_display_get_rotation(lvglDisplay) : 0;
    const bool hardwareRotation = display_has_capability(display, DISPLAY_CAPABILITY_CAP_SWAP_XY) &&
        display_has_capability(display, DISPLAY_CAPABILITY_CAP_MIRROR);
    const bool sideways = (rotation & 1) != 0;

    DoomScreen screen {};
    if (hardwareRotation) {
        screen.drawRotation = 0;
        screen.drawWidth = sideways ? nativeH : nativeW;
        screen.drawHeight = sideways ? nativeW : nativeH;
    } else {
        screen.drawRotation = rotation;
        screen.drawWidth = nativeW;
        screen.drawHeight = nativeH;
    }
    screen.touchRotation = rotation;
    screen.touchWidth = nativeW;
    screen.touchHeight = nativeH;
    LOG_I(TAG, "LVGL rotation %d (%s)", rotation, hardwareRotation ? "hardware" : "software");
    return screen;
}

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    struct Device* display_device;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display_device) != ERROR_NONE) {
        LOG_E(TAG, "No display device found");
        showErrorAndWait(app_instance_id, "No display device was found.");
        return 0;
    }

    // Played with a keyboard, or with on-screen touch controls.
    // Either way, exiting is done from the menu's "Quit Game".
    if (!device_exists_of_type(&KEYBOARD_TYPE) && !device_exists_of_type(&TOUCH_TYPE)) {
        LOG_E(TAG, "No keyboard or touch screen found");
        showErrorAndWait(app_instance_id, "Doom needs a keyboard or a touch screen.");
        device_put(display_device);
        return 0;
    }

    // How the user sees the display, read from LVGL while it still runs
    const DoomScreen screen = getScreen(display_device);

    // Stop LVGL so we can take exclusive control of the display
    module_stop(&lvgl_module);

    // Run the main logic
    LOG_I(TAG, "Running application");
    runApplication(display_device, screen);
    device_put(display_device);

    // Restart LVGL to resume rendering of regular apps
    if (!module_is_started(&lvgl_module)) {
        LOG_I(TAG, "Restarting LVGL");
        module_start(&lvgl_module);
    }

    // Doom stopped early (e.g. not enough memory): say why
    if (doomMain_GetError()[0] != '\0') {
        showErrorAndWait(app_instance_id, doomMain_GetError());
    }

    LOG_I(TAG, "Stopping application");

    return 0;
}

}
