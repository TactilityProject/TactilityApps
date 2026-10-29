// SPDX-License-Identifier: GPL-3.0-or-later
#include "StallProbe.h"

#include "../platform/Platform.h"

#include <esp_timer.h>

namespace maps {

namespace {

/** Above the app (5), the Wi-Fi driver's helpers and lwIP (18), below the Wi-Fi task and esp_timer. */
constexpr UBaseType_t PROBE_PRIORITY = 19;
constexpr uint32_t PROBE_STACK = 3072;

} // namespace

bool StallProbe::start() {
    stopRequested = false;
    for (int core = 0; core < 2; core++) {
        if (xTaskCreatePinnedToCore(probeMain, core == 0 ? "maps-probe0" : "maps-probe1", PROBE_STACK, this, PROBE_PRIORITY,
                &tasks[core], core) != pdPASS) {
            stop();
            return false;
        }
        running++;
    }
    logInfo("Stall probes running on both cores, priority %u, gaps over %u ms are logged", static_cast<unsigned>(PROBE_PRIORITY),
        static_cast<unsigned>(GAP_MS));
    return true;
}

void StallProbe::stop() {
    stopRequested = true;
    while (running > 0) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void StallProbe::watch(TaskHandle_t task, const char* name) {
    if (task != nullptr && watchedCount < MAX_WATCHED) {
        watched[watchedCount++] = { task, name };
    }
}

void StallProbe::logTaskStates(uint32_t stalledMs) {
    static const char* const STATES[] = { "running", "ready", "blocked", "suspended", "deleted", "invalid" };
    char line[160];
    int used = snprintf(line, sizeof(line), "Loop stalled %u ms:", static_cast<unsigned>(stalledMs));
    for (int i = 0; i < watchedCount && used < static_cast<int>(sizeof(line)); i++) {
        TaskStatus_t status {};
        vTaskGetInfo(watched[i].task, &status, pdFALSE, eInvalid);
        const int state = status.eCurrentState <= eInvalid ? static_cast<int>(status.eCurrentState) : 5;
        // The current priority above the base one means the task holds a lock a higher task waits for
        used += snprintf(line + used, sizeof(line) - used, " %s %s %u/%u", watched[i].name, STATES[state],
            static_cast<unsigned>(status.uxCurrentPriority), static_cast<unsigned>(status.uxBasePriority));
    }
    logInfo("%s", line);
}

void StallProbe::probeMain(void* context) {
    auto* probe = static_cast<StallProbe*>(context);
    const int core = xPortGetCoreID();
    const int64_t tickUs = static_cast<int64_t>(portTICK_PERIOD_MS) * 1000;
    int64_t last = esp_timer_get_time();
    int64_t nextMemory = last;
    while (!probe->stopRequested) {
        vTaskDelay(1);
        const int64_t now = esp_timer_get_time();
        const int64_t lateUs = now - last - tickUs;
        if (lateUs > static_cast<int64_t>(GAP_MS) * 1000) {
            logInfo("Stall on core %d: %u ms, from t=%u ms", core, static_cast<unsigned>(lateUs / 1000), static_cast<unsigned>(last / 1000));
        }
        last = now;
        // Core 1 looks at the map loop: when it stops coming round, what every app task is doing
        const uint32_t beat = probe->lastBeat;
        const uint32_t nowMs = static_cast<uint32_t>(now / 1000);
        if (core == 1 && beat != 0 && nowMs - beat > LOOP_STALL_MS && probe->reportedBeat != beat) {
            probe->reportedBeat = beat;
            probe->logTaskStates(nowMs - beat);
        }
        // Core 0 also samples memory once a second
        if (core == 0 && now >= nextMemory) {
            nextMemory = now + 1000000;
            logMemory("sample");
        }
    }
    probe->running--;
    vTaskDelete(nullptr);
}

} // namespace maps
