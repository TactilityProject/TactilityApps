#include "Application.h"
#include "DoomMain.h"

#include <tactility/log.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/touch.h>
#include <lvgl/lvgl.h>
#include <lvgl/module.h>
#include <tactility/module.h>

constexpr auto* TAG = "Doom";

// Shows a blocking error dialog and waits for it to close (so the user actually gets to read it)
// before the caller finishes this app - this app never creates a window of its own, so there's
// nothing else keeping it around for the dialog to be seen against.
static void showErrorAndWait(AppInstanceId appInstanceId, const char* message) {
    const char* argv[] = { "Error", message, "OK" };
    uint32_t dialogInstanceId = 0;
    app_start_for_result("tactility.alertdialog", 3, argv, appInstanceId, &dialogInstanceId);
    if (dialogInstanceId == 0) {
        return;
    }

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    struct AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    while (true) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        bool done = false;
        struct AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_RESULT && event.result.launch_id == dialogInstanceId) {
                app_manager_stop(dialogInstanceId);
                done = true;
                break;
            }
        }
        if (done) break;
    }

    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);
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
