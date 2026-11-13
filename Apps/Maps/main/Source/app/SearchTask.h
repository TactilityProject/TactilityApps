// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <cstdint>

namespace maps {

/**
 * Runs the search engine's work on its own task with a large stack in PSRAM (M6). The engine reads 4 KB key pages and
 * name groups into stack buffers; the map task has only a few KB of stack left (it overflowed on the T-Deck). The map
 * waits for each job, so it behaves as a direct call. Started on the first search, stopped when the app ends.
 */
class SearchTask {
public:
    using Job = void (*)(void* context);

    ~SearchTask() { stop(); }
    bool start();
    void stop();
    /** Runs job(context) on the task and waits for it. False when the task could not start. */
    bool run(Job job, void* context);
    /** The least stack the task has had left, in bytes (for the log). */
    uint32_t stackLeft() const { return task != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task)) : 0; }

private:
    static void taskMain(void* context);

    TaskHandle_t task = nullptr;
    SemaphoreHandle_t wake = nullptr;
    SemaphoreHandle_t done = nullptr;
    Job pending = nullptr;
    void* pendingContext = nullptr;
    bool quit = false;
};

} // namespace maps
