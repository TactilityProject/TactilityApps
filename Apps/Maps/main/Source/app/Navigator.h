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
    /**
     * A new route for a position that left the current one at progressM along it (0.6.0): first a short search back
     * to the current route a little ahead, keeping the rest of it; when that gives nothing sensible, the search to
     * the destination as request() does. False while a search runs.
     */
    bool requestRejoin(double fromLat, double fromLon, double toLat, double toLon, RouteProfile profile, float progressM);
    /** Whether the last route came from a rejoin (the rest of the old route kept), for the log. */
    bool lastWasRejoin() const { return rejoined; }
    /** Runs the pairs of a file (host/routing/export_pairs.py) with one profile or all (Count), logging each, for measurements. */
    bool bench(const char* pairsPath, RouteProfile profile);
    /**
     * Makes the route from the first track or route of a GPX file (0.6.0) instead of searching one; false while a
     * search runs. The result comes like a search's (state(), takeResult()).
     */
    bool requestTrack(const char* gpxPath);
    void cancel() { search.cancel(); }
    /** Settings, "Walk prefers paths": read by the next search. */
    void setPreferTrails(bool value) { search.setPreferTrails(value); }

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

    /** For the stack measurements of the debug script. */
    TaskHandle_t taskHandle() const { return task; }

private:
    enum class Job : uint8_t { None, Route, Bench, Sides, Track, Quit };

    static void taskMain(void* context);
    void loop();
    void runRoute();
    /** Makes the missing z14 graphs around both ends of a long route before the search starts. */
    static constexpr int MAX_LAYER_PREPARE = 96;
    static constexpr uint32_t LAYER_PREPARE_MS = 120000;
    void prepareEnds();
    /**
     * For a route over 50 km: the z10 graphs of the main-road layer along the way (tiles whose centre is within 30 km
     * of the straight line between the ends) that are not made yet, made before the search. Made during the search
     * each waited 10 s with little memory and some were given up, so the layer had holes and the route was "too long"
     * (Kosice - Bratislava on the T-Deck, 15 Nov 2026). At most MAX_LAYER_PREPARE tiles and LAYER_PREPARE_MS.
     */
    void prepareLayer();
    /** Makes the graphs of these tiles that are missing (the render lanes, urgent), waiting at most maxMs. */
    int prepareGraphs(const TileKey* tiles, int count, uint32_t maxMs, const char* what);
    /** The short search back to the current route; true when `next` holds the spliced route. */
    bool runRejoin(GraphSource& source, Route& next);
    void runBench();
    void runSides();
    void runTrack();
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
    /** Progress along the current route for a rejoin; negative: a plain search. */
    float requestProgress = -1.0f;
    bool rejoined = false;
    bool rejoinCancelled = false;
    /** The route change count the rejoin was spliced against: the splice is installed only if it still holds. */
    uint32_t rejoinGeneration = 0;
    RouteProfile requestProfile = RouteProfile::Walk;
};

} // namespace maps
