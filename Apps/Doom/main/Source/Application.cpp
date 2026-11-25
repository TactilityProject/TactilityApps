#include "Application.h"
#include "DoomMain.h"


#include <tactility/log.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif
#include <tactility/freertos/freertos.h>
#include <tactility/freertos/semphr.h>
#include <tactility/freertos/task.h>

constexpr auto* TAG = "Application";

// Doom's BSP traversal recurses deeply; the default app task stack is too small.
constexpr uint32_t DOOM_TASK_STACK_SIZE = 48 * 1024;

struct DoomTaskParams {
    Device* display;
    DoomScreen screen;
    SemaphoreHandle_t doneSem;
};

static void doomTask(void* arg) {
    auto* params = static_cast<DoomTaskParams*>(arg);

    doomMain_Run(params->display, params->screen);

    xSemaphoreGive(params->doneSem);
    // runApplication() deletes this task (vTaskDeleteWithCaps() frees the stack it allocated).
    vTaskSuspend(nullptr);
}

void runApplication(Device* display, const DoomScreen& screen) {
    SemaphoreHandle_t doneSem = xSemaphoreCreateBinary();
    if (doneSem == nullptr) {
        LOG_E(TAG, "Failed to create shutdown semaphore");
        return;
    }

    DoomTaskParams params = { display, screen, doneSem };

    // Higher priority so this task isn't starved by background OS housekeeping tasks,
    // which can otherwise delay vTaskDelay(1ms) wakeups long enough to trip the
    // interrupt watchdog (HP_SYS_HP_WDT_RESET).
    // Core 1 on dual-core chips; single-core chips (e.g. ESP32-C6) only have core 0.
    TaskHandle_t task = nullptr;
#ifdef ESP_PLATFORM
    // Plain xTaskCreate() needs the stack as one block of internal RAM, which fails on devices
    // like the T-Deck Plus whose internal heap is too fragmented by then. Internal RAM is
    // preferred (faster); PSRAM is the fallback.
    const int core = portNUM_PROCESSORS > 1 ? 1 : 0;
    if (xTaskCreatePinnedToCoreWithCaps(doomTask, "doom", DOOM_TASK_STACK_SIZE, &params, 5, &task, core, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        LOG_W(TAG, "Not enough internal RAM for the doom task stack, using PSRAM");
        if (xTaskCreatePinnedToCoreWithCaps(doomTask, "doom", DOOM_TASK_STACK_SIZE, &params, 5, &task, core, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
            task = nullptr;
        }
    }
#else
    // The POSIX FreeRTOS port has no core affinity.
    if (xTaskCreate(doomTask, "doom", DOOM_TASK_STACK_SIZE, &params, 5, &task) != pdPASS) {
        task = nullptr;
    }
#endif
    if (task == nullptr) {
        LOG_E(TAG, "Failed to create doom task");
        vSemaphoreDelete(doneSem);
        return;
    }

    LOG_I(TAG, "Doom started - quit from the menu to exit");

    // The doom task runs until the player picks Quit Game in the menu
    xSemaphoreTake(doneSem, portMAX_DELAY);

    // The doom task suspends itself right after signalling doneSem
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(task);
#else
    vTaskDelete(task);
#endif

    vSemaphoreDelete(doneSem);
    LOG_I(TAG, "Doom ended");
}
