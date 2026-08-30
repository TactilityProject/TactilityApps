#include "Application.h"
#include "DoomEsp.h"

#include <tactility/drivers/pointer.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

constexpr auto* TAG = "Application";

// Doom's BSP traversal recurses deeply; the default app task stack is too small.
constexpr uint32_t DOOM_TASK_STACK_SIZE = 48 * 1024;

// Touch is polled while the doom task runs, so it doesn't need to be too fast.
constexpr int EXIT_POLL_DELAY_MS = 100;

struct DoomTaskParams {
    Device* display;
    SemaphoreHandle_t doneSem;
};

static void doomTask(void* arg) {
    auto* params = static_cast<DoomTaskParams*>(arg);

    doomEsp_Run(params->display);

    xSemaphoreGive(params->doneSem);
    vTaskDelete(nullptr);
}

static bool isTouched(Device* touch) {
    if (pointer_read_data(touch, 0) != ERROR_NONE) {
        return false;
    }
    uint16_t x, y, strength;
    uint8_t pointCount = 0;
    return pointer_get_touched_points(touch, &x, &y, &strength, &pointCount, 1);
}

void runApplication(Device* display, Device* touch) {
    SemaphoreHandle_t doneSem = xSemaphoreCreateBinary();
    if (doneSem == nullptr) {
        ESP_LOGE(TAG, "Failed to create shutdown semaphore");
        return;
    }

    DoomTaskParams params = { display, doneSem };

    // Higher priority so this task isn't starved by background OS housekeeping tasks,
    // which can otherwise delay vTaskDelay(1ms) wakeups long enough to trip the
    // interrupt watchdog (HP_SYS_HP_WDT_RESET).
    BaseType_t created = xTaskCreatePinnedToCore(doomTask, "doom", DOOM_TASK_STACK_SIZE, &params, 5, nullptr, 1);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create doom task");
        vSemaphoreDelete(doneSem);
        return;
    }

    ESP_LOGI(TAG, "Doom started - touch to exit");

    // Wait until either the doom task exits on its own, or the user touches the screen
    while (xSemaphoreTake(doneSem, pdMS_TO_TICKS(EXIT_POLL_DELAY_MS)) == pdFALSE) {
        if (isTouched(touch)) {
            doomEsp_RequestStop();
            xSemaphoreTake(doneSem, portMAX_DELAY);
            break;
        }
    }

    vSemaphoreDelete(doneSem);
    ESP_LOGI(TAG, "Doom ended");
}
