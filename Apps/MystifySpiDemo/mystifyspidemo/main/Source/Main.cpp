#include "Application.h"

#include <esp_log.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>
#include <input/AnyInput.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/module.h>
#include <lvgl/module.h>

constexpr auto* TAG = "MystifyDemo";

// Shows a blocking error dialog and waits for it to close before the caller finishes this app.
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
            if (event.type == APP_EVENT_CLOSE) {
                done = true;
                break;
            }
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
