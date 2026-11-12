// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include "../tiles/DownloadPlan.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace maps {

/**
 * Counts the tiles of a download plan that are already in the browsed region, for the download screen's estimate
 * ("1240 tiles, 310 already on the card"): those move into the region instead of being downloaded.
 *
 * The count lists only the browsed column folders inside the plan (one folder read per tile column, no file is
 * opened), on the I/O task at background priority, from a short-lived task of its own so the map loop never waits.
 * The map loop calls request() with the plan on screen and poll() every pass.
 */
class BrowsedCounter {
public:
    ~BrowsedCounter();
    void configure(IoTask& ioTask, const char* root);
    /** The plan on screen now; a count starts once the plan has stayed the same for SETTLE_MS. */
    void request(const DownloadPlan& plan, uint32_t nowMs);
    /** Starts and reaps the counting task; true when a new result came in (redraw). */
    bool poll(uint32_t nowMs);
    /** The count for this plan, when it is known. */
    bool count(const DownloadPlan& plan, int64_t& out) const;
    void stop();

    static constexpr uint32_t SETTLE_MS = 400;

private:
    static void taskMain(void* context);
    static bool samePlan(const DownloadPlan& a, const DownloadPlan& b);

    IoTask* io = nullptr;
    char rootPath[160] = "";
    TaskHandle_t task = nullptr;
    std::atomic<bool> finished { false };
    /** The plan wanted on screen, since when, and the one being counted. */
    DownloadPlan wanted {};
    bool hasWanted = false;
    uint32_t wantedSince = 0;
    DownloadPlan counting {};
    int64_t countingResult = 0;
    bool countingComplete = false;
    /** The last finished count. */
    DownloadPlan counted {};
    int64_t countedResult = 0;
    bool hasCount = false;
};

} // namespace maps
