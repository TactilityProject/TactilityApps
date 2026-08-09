#include "Application.h"
#include "drivers/DisplayDriver.h"
#include "drivers/TouchDriver.h"

#include <esp_log.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/scheduler.h>

#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/pointer.h>
#include <lvgl/lvgl.h>
#include <lvgl/module.h>
#include <tactility/module.h>

constexpr auto TAG = "Main";

// Shows a blocking error dialog and waits for it to close (so the user actually gets to read it)
// before the caller finishes this app - this app never creates a window of its own, so there's
// nothing else keeping it around for the dialog to be seen against.
static void showErrorAndWait(AppInstanceId appInstanceId, const char* message) {
    const char* argv[] = { "Error", message, "OK" };
    uint32_t dialogInstanceId = 0;
    app_manager_start_for_result("AlertDialog", appInstanceId, 3, argv, &dialogInstanceId);
    if (dialogInstanceId == 0) {
        return;
    }

    struct AppEventSubscription sub {};
    sub.app_instance_id = appInstanceId;
    app_event_subscribe(&sub);

    while (true) {
        struct AppEvent event {};
        if (app_event_await(&sub, &event, portMAX_DELAY) != ERROR_NONE) {
            break;
        }
        if (event.type == APP_EVENT_RESULT && event.result.launch_id == dialogInstanceId) {
            app_manager_stop(dialogInstanceId);
            break;
        }
    }

    app_event_unsubscribe(&sub);
}

extern "C" {

int main(int argc, char* argv[]) {
    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    struct Device* display_device;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display_device) != ERROR_NONE) {
        ESP_LOGE(TAG, "No display device found");
        showErrorAndWait(app_instance_id, "No display device was found.");
        app_manager_finish(app_instance_id);
        return 0;
    }

    struct Device* touch_device;
    if (device_get_first_active_by_type(&POINTER_TYPE, &touch_device) != ERROR_NONE) {
        ESP_LOGE(TAG, "No touch device found");
        device_put(display_device);
        showErrorAndWait(app_instance_id, "No touch device was found.");
        app_manager_finish(app_instance_id);
        return 0;
    }

    // Stop LVGL first (because it's currently using the drivers we want to use)
    module_stop(&lvgl_module);

    ESP_LOGI(TAG, "Creating display driver");
    auto display = new DisplayDriver(display_device);
    device_put(display_device);

    ESP_LOGI(TAG, "Creating touch driver");
    auto touch = new TouchDriver(touch_device);
    device_put(touch_device);

    // Run the main logic
    ESP_LOGI(TAG, "Running application");
    runApplication(display, touch);

    ESP_LOGI(TAG, "Cleanup display driver");
    delete display;

    ESP_LOGI(TAG, "Cleanup touch driver");
    delete touch;

    // Restart LVGL to resume rendering of regular apps
    if (!module_is_started(&lvgl_module)) {
        ESP_LOGI(TAG, "Restarting LVGL");
        module_start(&lvgl_module);
    }

    ESP_LOGI(TAG, "Stopping application");
    app_manager_finish(app_instance_id);

    return 0;
}

}
