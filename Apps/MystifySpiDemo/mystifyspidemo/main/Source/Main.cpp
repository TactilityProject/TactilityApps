#include "Application.h"

#include <lvgl_window_manager/dialog.h>
#include <lvgl_window_manager/window_manager.h>
#include <esp_log.h>

#include <app/event.h>
#include <app/scheduler.h>
#include <input/AnyInput.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/module.h>
#include <lvgl/module.h>

constexpr auto* TAG = "MystifyDemo";

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
    (void)argc;
    (void)argv;

    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    struct Device* display_device = nullptr;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display_device) != ERROR_NONE) {
        ESP_LOGE(TAG, "No display device found");
        showErrorAndWait(app_instance_id, "No display device was found.");
        return 0;
    }

    const char* error = nullptr;
    {
        AnyInput input;
        if (!input.available()) {
            error = "No input device was found.";
        } else {
            // The app draws to both displays directly, so LVGL has to release the primary panel.
            module_stop(&lvgl_module);
            error = runApplication(display_device, input);
        }
    }

    device_put(display_device);
    if (!module_is_started(&lvgl_module)) {
        module_start(&lvgl_module);
    }

    if (error != nullptr) {
        ESP_LOGE(TAG, "%s", error);
        showErrorAndWait(app_instance_id, error);
    }

    return 0;
}

}
