// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrowsedCounter.h"

#include "../platform/Platform.h"
#include "../tiles/TileStore.h"

#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#include <dirent.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

constexpr uint32_t TASK_STACK = 4096;
constexpr UBaseType_t TASK_PRIORITY = 2;

struct CountJob {
    const char* root;
    DownloadPlan plan;
    int64_t count;
    bool complete;
};

/** Reads a folder name or file stem as a tile number; -1 when it is not one. */
int tileNumber(const char* name, const char* suffix) {
    char* end = nullptr;
    const long value = strtol(name, &end, 10);
    if (end == name || value < 0 || strcmp(end, suffix) != 0) {
        return -1;
    }
    return static_cast<int>(value);
}

bool countJob(IoTask& io, void* context) {
    auto* job = static_cast<CountJob*>(context);
    job->count = 0;
    job->complete = false;
    for (int z = job->plan.minZoom; z <= job->plan.maxZoom; z++) {
        const TileRange range = tileRange(job->plan.box, z);
        char folder[220];
        snprintf(folder, sizeof(folder), "%s/maps/%s/%d", job->root, BROWSE_REGION, z);
        DIR* columns = opendir(folder);
        if (columns == nullptr) {
            continue;
        }
        // The column folders first, then the files of the ones inside the plan: never a file opened
        int inside[96];
        int insideCount = 0;
        bool more = false;
        for (dirent* entry = readdir(columns); entry != nullptr; entry = readdir(columns)) {
            const int x = tileNumber(entry->d_name, "");
            if (x >= range.minX && x <= range.maxX) {
                if (insideCount < 96) {
                    inside[insideCount++] = x;
                } else {
                    more = true;
                }
            }
        }
        closedir(columns);
        (void)more;
        for (int i = 0; i < insideCount; i++) {
            // Tiles on screen come first: give the card back and say the count is not complete
            if (io.shouldYield()) {
                return true;
            }
            char column[240];
            snprintf(column, sizeof(column), "%s/%d", folder, inside[i]);
            DIR* files = opendir(column);
            if (files == nullptr) {
                continue;
            }
            for (dirent* entry = readdir(files); entry != nullptr; entry = readdir(files)) {
                const int y = tileNumber(entry->d_name, ".pbf");
                if (y >= range.minY && y <= range.maxY) {
                    job->count++;
                }
            }
            closedir(files);
        }
    }
    job->complete = true;
    return true;
}

} // namespace

BrowsedCounter::~BrowsedCounter() {
    stop();
}

void BrowsedCounter::configure(IoTask& ioTask, const char* root) {
    io = &ioTask;
    snprintf(rootPath, sizeof(rootPath), "%s", root);
}

bool BrowsedCounter::samePlan(const DownloadPlan& a, const DownloadPlan& b) {
    return a.minZoom == b.minZoom && a.maxZoom == b.maxZoom && a.box.west == b.box.west && a.box.east == b.box.east &&
        a.box.south == b.box.south && a.box.north == b.box.north;
}

void BrowsedCounter::request(const DownloadPlan& plan, uint32_t nowMs) {
    if (!hasWanted || !samePlan(plan, wanted)) {
        wanted = plan;
        hasWanted = true;
        wantedSince = nowMs;
    }
}

bool BrowsedCounter::count(const DownloadPlan& plan, int64_t& out) const {
    if (!hasCount || !samePlan(plan, counted)) {
        return false;
    }
    out = countedResult;
    return true;
}

void BrowsedCounter::taskMain(void* context) {
    auto* counter = static_cast<BrowsedCounter*>(context);
    CountJob job { counter->rootPath, counter->counting, 0, false };
    // A count that gave way to the map is tried again a little later
    for (int attempt = 0; attempt < 40; attempt++) {
        if (!counter->io->run(IoPriority::Background, countJob, &job) || job.complete) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    counter->countingResult = job.count;
    counter->countingComplete = job.complete;
    counter->finished = true;
    // Deleted by poll() or stop(): a task cannot free its own PSRAM stack
    vTaskSuspend(nullptr);
}

bool BrowsedCounter::poll(uint32_t nowMs) {
    bool changed = false;
    if (task != nullptr && finished) {
        vTaskDeleteWithCaps(task);
        task = nullptr;
        if (!countingComplete) {
            // Gave way to the map too often: try again after a pause
            wantedSince = nowMs;
        } else {
            counted = counting;
            countedResult = countingResult;
            hasCount = true;
            changed = true;
        }
    }
    if (task == nullptr && io != nullptr && hasWanted && nowMs - wantedSince >= SETTLE_MS && !(hasCount && samePlan(wanted, counted)) &&
        wanted.fits && wanted.tiles > 0) {
        counting = wanted;
        finished = false;
        if (xTaskCreatePinnedToCoreWithCaps(taskMain, "maps-count", TASK_STACK, this, TASK_PRIORITY, &task, tskNO_AFFINITY,
                MALLOC_CAP_SPIRAM) != pdPASS) {
            task = nullptr;
            // Not counted: the estimate simply shows no tiles on the card
            hasWanted = false;
        }
    }
    return changed;
}

void BrowsedCounter::stop() {
    if (task == nullptr) {
        return;
    }
    while (!finished) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDeleteWithCaps(task);
    task = nullptr;
}

} // namespace maps
