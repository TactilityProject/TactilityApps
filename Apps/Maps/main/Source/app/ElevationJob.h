// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../render/ElevationProfile.h"
#include "../route/RouteSearch.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

namespace maps {

class TileWorker;

/**
 * The elevation profile of a found route, read from the terrain files in a task of its own: a few samples per card
 * job, so tiles on screen are not held up. The route card shows it once Done.
 */
class ElevationJob {
public:
    enum class State : uint8_t {
        Idle,
        Busy,
        Done
    };

    ~ElevationJob();
    /** Samples the route (copied now); a running job is stopped first. */
    void start(TileWorker& worker, const Route& route);
    /** Stops a running job (a new route, the card closed); the state goes back to Idle. */
    void cancel();
    State state() const { return currentState.load(); }
    /** After Done: the profile; count 0 where the route has no terrain. */
    const ElevationProfile& profile() const { return result; }
    /** Marks a Done profile as shown, so it is applied once. */
    void acknowledge();

private:
    static void taskMain(void* context);
    void run();
    void waitStopped();

    TileWorker* worker = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<State> currentState = State::Idle;
    std::atomic<bool> stopRequested = false;
    std::atomic<bool> running = false;
    int sampleCount = 0;
    double lats[ElevationProfile::MAX_SAMPLES] {};
    double lons[ElevationProfile::MAX_SAMPLES] {};
    ElevationProfile result;
};

} // namespace maps
