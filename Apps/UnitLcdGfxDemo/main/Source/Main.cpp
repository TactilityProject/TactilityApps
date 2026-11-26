#include "Application.h"

#include <lvgl_window_manager/dialog.h>
#include <lvgl_window_manager/window_manager.h>
#include <esp_log.h>

#include <app/event.h>
#include <app/scheduler.h>

#include <gfx/Gfx.h>
#include <input/AnyInput.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <lvgl/lvgl.h>
#include <lvgl/module.h>
#include <tactility/module.h>

constexpr auto TAG = "UnitLcdGfxDemo";

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

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    struct Device* display_device;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display_device) != ERROR_NONE) {
        ESP_LOGE(TAG, "No display device found");
        showErrorAndWait(app_instance_id, "No display device was found.");
        return 0;
    }

    auto input = new AnyInput();
    if (!input->available()) {
        delete input;
        showErrorAndWait(app_instance_id, "No input device was found.");
        return 0;
    }

    // The demo uses the primary display directly while the LCD unit is driven through a sink.
    // Target creation is deferred into runApplication() so the LCD frame can be allocated first.
    module_stop(&lvgl_module);

    runApplication(display_device, input);

    device_put(display_device);
    delete input;

    if (!module_is_started(&lvgl_module)) {
        module_start(&lvgl_module);
    }

    return 0;
}

}
