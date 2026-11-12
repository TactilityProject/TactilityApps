// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include "../tiles/TilePaths.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace maps {

/**
 * Builds the offline search indexes in idle time (M6): for every region on the card whose index is missing or older
 * than its tiles (maps/<region>/search.inf holds the meta.json version, date and tile count it was built from), the
 * region the map shows first. Uses SearchStreamBuilder on its own low-priority task with the scratch files in
 * maps/<region>/srch; all card access goes through the I/O task at background priority, so tiles on screen and
 * downloads come first. A region of at most 15 x 15 z14 tiles gets maps/<region>/search.idx; a larger one is split
 * into blocks of 15 x 15 tiles from its first tile, maps/<region>/search/<bx>_<by>.idx (15, not 16: a block then stays
 * below 65536 units with the tiles' buffers, so its points keep 4 bytes).
 */
class SearchIndexJob {
public:
    enum class State : uint8_t {
        Idle,
        Busy,
        Done,
        Failed
    };

    ~SearchIndexJob();
    /**
     * Starts the background build; first is the region to build first (the map centre's), may be null. force builds
     * even indexes that are current, only that one region when only is set (debug scripts). False while a build runs.
     */
    bool start(IoTask& io, const char* rootPath, const char* first, bool force = false, const char* only = nullptr);
    /** While paused the build waits between steps with its files closed (a download runs). */
    void pause(bool value) { paused = value; }
    /** Stops a running build (its scratch files are removed) and waits for the task. */
    void stop();
    State state() const { return currentState.load(); }
    /** Progress of the region being built (steps of the build), for "Building search index N/M". */
    int32_t done() const { return progressDone.load(); }
    int32_t total() const { return progressTotal.load(); }
    /** After Done or Failed: back to Idle. */
    void acknowledge();
    /** Whether an index was written by the last build (the map then reloads the search). */
    bool builtAny() const { return written > 0; }
    /** One report line at a time for the log (measurements), nullptr when there is none; the line stays until the next call. */
    const char* nextReport();
    /** The lowest free PSRAM seen during the build, in bytes. */
    uint32_t lowestFreePsram() const { return lowestFree; }

    /** Called by the builder between steps: waits while paused, false when stopping. */
    bool keepGoing(int32_t done, int32_t total);
    void report(const char* format, ...);

private:
    static void taskMain(void* context);
    void run();
    bool buildRegion(const char* region);

    IoTask* io = nullptr;
    TaskHandle_t task = nullptr;
    std::atomic<State> currentState = State::Idle;
    std::atomic<bool> paused = false;
    std::atomic<bool> stopping = false;
    std::atomic<int32_t> progressDone = 0;
    std::atomic<int32_t> progressTotal = 0;
    char root[PATH_MAX_LENGTH] = "";
    char firstRegion[16] = "";
    char onlyRegion[16] = "";
    bool forceBuild = false;
    int written = 0;
    uint32_t lowestFree = 0;
    /** Report lines: written by the task, read by the map loop (a small ring, guarded by the counters). */
    static constexpr int REPORT_LINES = 24;
    char reports[REPORT_LINES][200] = {};
    std::atomic<uint32_t> reportWritten = 0;
    std::atomic<uint32_t> reportRead = 0;
    char reportOut[200] = "";
};

} // namespace maps
