// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace maps {

/**
 * A developer tool for finding stalls (setting dev_probe=1 in settings.txt). One small task per core, at a
 * priority above every app task and the network stack, wakes every tick and logs when it was kept from running
 * for longer than GAP_MS. If the probes stall together with the app, the core was held below the scheduler
 * (interrupts or the flash cache off, as during a flash write); if only the app stalls, a task of higher priority
 * than the app ran or the app waited for a lock. The probe on core 0 also logs free memory once a second.
 */
class StallProbe {
public:
    static constexpr uint32_t GAP_MS = 40;
    /** The map loop counts as stalled when it has not come round for this long. */
    static constexpr uint32_t LOOP_STALL_MS = 300;
    static constexpr int MAX_WATCHED = 8;

    bool start();
    void stop();
    /** A task whose state is logged when the map loop stalls. Call before start(). */
    void watch(TaskHandle_t task, const char* name);
    /** Called by the map loop on every round. */
    void heartbeat(uint32_t nowMs) { lastBeat = nowMs; }

private:
    static void probeMain(void* context);
    void logTaskStates(uint32_t stalledMs);

    struct Watched {
        TaskHandle_t task;
        const char* name;
    };
    Watched watched[MAX_WATCHED] {};
    int watchedCount = 0;
    std::atomic<uint32_t> lastBeat = 0;
    uint32_t reportedBeat = 0;

    TaskHandle_t tasks[2] {};
    std::atomic<bool> stopRequested = false;
    std::atomic<int> running = 0;
};

} // namespace maps
