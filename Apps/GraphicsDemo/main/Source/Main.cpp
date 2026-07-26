#include "Application.h"
#include "drivers/DisplayDriver.h"
#include "drivers/TouchDriver.h"

#include <esp_log.h>

#include <tt_app.h>
#include <tt_app_alertdialog.h>

#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/pointer.h>
#include <lvgl/lvgl.h>
#include <lvgl/module.h>
#include <tactility/module.h>

constexpr auto TAG = "Main";

static void onCreate(AppHandle appHandle, void* data) {
    struct Device* display_device;
    if (device_get_first_active_by_type(&DISPLAY_TYPE, &display_device) != ERROR_NONE) {
        ESP_LOGE(TAG, "No display device found");
        tt_app_stop();
        tt_app_alertdialog_start("Error", "No display device was found.", nullptr, 0);
        return;
    }

    struct Device* touch_device;
    if (device_get_first_active_by_type(&POINTER_TYPE, &touch_device) != ERROR_NONE) {
        ESP_LOGE(TAG, "No touch device found");
        device_put(display_device);
        tt_app_stop();
        tt_app_alertdialog_start("Error", "No touch device was found.", nullptr, 0);
        return;
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

    ESP_LOGI(TAG, "Stopping application");
    tt_app_stop();
}

static void onDestroy(AppHandle appHandle, void* data) {
    // Restart LVGL to resume rendering of regular apps
    if (!module_is_started(&lvgl_module)) {
        ESP_LOGI(TAG, "Restarting LVGL");
        module_start(&lvgl_module);
    }
}

extern "C" {

int main(int argc, char* argv[]) {
    tt_app_register((AppRegistration) {
        .onCreate = onCreate,
        .onDestroy = onDestroy
    });
    return 0;
}

}
