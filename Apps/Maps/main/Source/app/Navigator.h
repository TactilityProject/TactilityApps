// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include "../route/RouteSearch.h"
#include "../tiles/TilePaths.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace maps {

class TileStore;
class TileWorker;

/**
 * Finds routes on its own task (M5c-2) and keeps the current route, saved in route.bin so it survives a restart. The
 * search borrows memory from the tile cache while it runs (TileWorker::relieveMemory) and gives it back afterwards.
 * The map loop asks for a route with request(), then polls state(); the route is read between lock() and unlock().
 */
class Navigator {
public:
    enum class State : uint8_t { Idle, Searching, Done, Failed };

    /** The memory a search may use: its node states (65,536), queue and the tiles' graphs. */
    static constexpr size_t SEARCH_BYTES = 2560 * 1024;

    bool start(IoTask& io, TileStore& store, TileWorker& worker, const char* dataPath);
    void stop();

    /** Starts a search; false while one runs. The result replaces the current route only when one is found. */
    bool request(double fromLat, double fromLon, double toLat, double toLon, RouteProfile profile);
    /** Runs the pairs of a file (host/routing/export_pairs.py) with one profile or all (Count), logging each, for measurements. */
    bool bench(const char* pairsPath, RouteProfile profile);
    void cancel() { search.cancel(); }

    State state() const { return currentState.load(); }
    /** The last search's outcome and its message, for the toast. Reading it sets the state back to Idle. */
    RouteResult takeResult();
    const RouteStats& lastStats() const { return stats; }
    /** Changes whenever the route changed (found, cleared or loaded). */
    uint32_t generation() const { return changes.load(); }

    bool hasRoute() const { return routeReady.load(); }
    void lock();
    void unlock();
    /** The current route: only between lock() and unlock(). */
    const Route& route() const { return routes[current]; }
    /** Drops the route and deletes route.bin. */
    void clearRoute();

private:
    enum class Job : uint8_t { None, Route, Bench, Quit };

    static void taskMain(void* context);
    void loop();
    void runRoute();
    void runBench();
    bool takeMemory();
    void giveMemory();
    bool saveRoute(const Route& route);
    bool loadRoute();

    class CardSource;

    IoTask* io = nullptr;
    TileStore* store = nullptr;
    TileWorker* worker = nullptr;
    char routePath[PATH_MAX_LENGTH] = "";
    char benchPath[PATH_MAX_LENGTH] = "";
    TaskHandle_t task = nullptr;
    SemaphoreHandle_t wake = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t stopped = nullptr;
    std::atomic<Job> job { Job::None };
    std::atomic<State> currentState { State::Idle };
    std::atomic<bool> routeReady { false };
    std::atomic<uint32_t> changes { 0 };
    RouteSearch search;
    /** Two routes: the search fills the other one, which then becomes the current one. */
    Route routes[2];
    int current = 0;
    RouteResult result = RouteResult::Found;
    RouteStats stats {};
    double requestFrom[2] {};
    double requestTo[2] {};
    RouteProfile requestProfile = RouteProfile::Walk;
};

} // namespace maps
