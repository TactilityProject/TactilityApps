// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchTask.h"

#include "../platform/Platform.h"

#include <esp_heap_caps.h>

namespace maps {

namespace {

constexpr uint32_t TASK_STACK = 16 * 1024;
constexpr UBaseType_t TASK_PRIORITY = 4;

} // namespace

bool SearchTask::start() {
    if (task != nullptr) {
        return true;
    }
    wake = xSemaphoreCreateBinary();
    done = xSemaphoreCreateBinary();
    if (wake == nullptr || done == nullptr) {
        stop();
        return false;
    }
    quit = false;
    if (xTaskCreatePinnedToCoreWithCaps(taskMain, "maps-search", TASK_STACK, this, TASK_PRIORITY, &task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        task = nullptr;
        stop();
        logError("Search: no task");
        return false;
    }
    return true;
}

void SearchTask::stop() {
    if (task != nullptr) {
        quit = true;
        xSemaphoreGive(wake);
        xSemaphoreTake(done, portMAX_DELAY);
        vTaskDeleteWithCaps(task);
        task = nullptr;
    }
    if (wake != nullptr) {
        vSemaphoreDelete(wake);
        wake = nullptr;
    }
    if (done != nullptr) {
        vSemaphoreDelete(done);
        done = nullptr;
    }
}

bool SearchTask::run(Job job, void* context) {
    if (!start()) {
        return false;
    }
    pending = job;
    pendingContext = context;
    xSemaphoreGive(wake);
    xSemaphoreTake(done, portMAX_DELAY);
    return true;
}

void SearchTask::taskMain(void* context) {
    auto* self = static_cast<SearchTask*>(context);
    while (true) {
        xSemaphoreTake(self->wake, portMAX_DELAY);
        if (self->quit) {
            break;
        }
        self->pending(self->pendingContext);
        xSemaphoreGive(self->done);
    }
    xSemaphoreGive(self->done);
    vTaskSuspend(nullptr);
}

} // namespace maps
