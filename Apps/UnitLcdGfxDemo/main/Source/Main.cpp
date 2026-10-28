#include "Application.h"

#include <esp_log.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>

#include <gfx/Gfx.h>
#include <input/AnyInput.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <lvgl/lvgl.h>
#include <lvgl/module.h>
#include <tactility/module.h>

constexpr auto TAG = "UnitLcdGfxDemo";

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
