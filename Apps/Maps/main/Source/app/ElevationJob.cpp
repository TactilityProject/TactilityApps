// SPDX-License-Identifier: GPL-3.0-or-later
#include "ElevationJob.h"

#include "TileWorker.h"

#include "../platform/Platform.h"

#include <esp_heap_caps.h>

namespace maps {

namespace {

constexpr uint32_t TASK_STACK = 6144;
constexpr UBaseType_t TASK_PRIORITY = 2;
/** Samples per card job: about 0.1-0.3 s each, then tiles on screen get the card. */
constexpr int SAMPLES_PER_JOB = 12;

} // namespace

ElevationJob::~ElevationJob() {
    cancel();
}

void ElevationJob::waitStopped() {
    stopRequested = true;
    for (int i = 0; i < 200 && running.load(); i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (task != nullptr && !running.load()) {
        vTaskDeleteWithCaps(task);
        task = nullptr;
    }
}

void ElevationJob::cancel() {
    if (task != nullptr) {
        waitStopped();
    }
    currentState = State::Idle;
}

void ElevationJob::acknowledge() {
    if (currentState == State::Done) {
        currentState = State::Idle;
    }
}

void ElevationJob::start(TileWorker& tileWorker, const Route& route) {
    cancel();
    worker = &tileWorker;
    result.clear();
    // Evenly spaced by distance along the route's line
    const size_t points = route.points.size();
    sampleCount = 0;
    if (points < 2 || route.distances.size() != points || route.lengthM <= 0.0f) {
        return;
    }
    const int wanted = elevationSampleCount(route.lengthM);
    size_t at = 0;
    for (int i = 0; i < wanted; i++) {
        const float distance = route.lengthM * static_cast<float>(i) / static_cast<float>(wanted - 1);
        while (at + 1 < points - 1 && route.distances[at + 1] < distance) {
            at++;
        }
        const float d0 = route.distances[at];
        const float d1 = route.distances[at + 1];
        const float t = d1 > d0 ? (distance - d0) / (d1 - d0) : 0.0f;
        const RoutePoint& a = route.points[at];
        const RoutePoint& b = route.points[at + 1];
        RoutePoint p { static_cast<int32_t>(a.x + (b.x - a.x) * t), static_cast<int32_t>(a.y + (b.y - a.y) * t) };
        routePointTo(p, lats[sampleCount], lons[sampleCount]);
        result.distanceM[sampleCount] = distance;
        sampleCount++;
    }
    stopRequested = false;
    running = true;
    currentState = State::Busy;
    if (xTaskCreatePinnedToCoreWithCaps(taskMain, "maps-elev", TASK_STACK, this, TASK_PRIORITY, &task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        running = false;
        task = nullptr;
        currentState = State::Idle;
    }
}

void ElevationJob::taskMain(void* context) {
    auto* job = static_cast<ElevationJob*>(context);
    job->run();
    job->running = false;
    vTaskSuspend(nullptr);
}

void ElevationJob::run() {
    float heights[SAMPLES_PER_JOB];
    bool found[SAMPLES_PER_JOB];
    int kept = 0;
    // A whole terrain file in memory: one card read per file instead of two small ones per point (10 s for 150)
    auto* file = static_cast<uint8_t*>(largeAlloc(TERRAIN_FILE_BYTES));
    TileKey held { -1, 0, 0 };
    for (int first = 0; first < sampleCount && !stopRequested; first += SAMPLES_PER_JOB) {
        const int count = sampleCount - first < SAMPLES_PER_JOB ? sampleCount - first : SAMPLES_PER_JOB;
        if (file != nullptr) {
            // At the priority of the ring around the screen, a batch may not start when the view changes: tried again
            int tries = 0;
            while (!worker->heightsAt(lats + first, lons + first, count, heights, found, file, held) && !stopRequested && ++tries < 20) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
        } else {
            worker->heightsAt(lats + first, lons + first, count, heights, found);
        }
        for (int i = 0; i < count; i++) {
            // Points without terrain are left out: the chart joins the ones around them
            if (found[i]) {
                result.distanceM[kept] = result.distanceM[first + i];
                result.heightM[kept] = heights[i];
                kept++;
            }
        }
    }
    largeFree(file);
    if (stopRequested) {
        return;
    }
    // Most of the route without terrain: no profile rather than a misleading one
    result.count = kept * 2 >= sampleCount ? kept : 0;
    result.finish();
    logInfo("Elevation: %d of %d points with terrain, climb %d m, descent %d m", kept, sampleCount, static_cast<int>(result.ascentM),
        static_cast<int>(result.descentM));
    currentState = State::Done;
}

} // namespace maps
