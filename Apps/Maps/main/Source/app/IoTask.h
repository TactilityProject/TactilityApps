// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/ByteInput.h"
#include "../tiles/IoQueue.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace maps {

class IoTask;

/** A piece of SD card work. It runs on the I/O task and returns whether it succeeded. */
using IoJob = bool (*)(IoTask& io, void* context);

/**
 * The only task that reads or writes the SD card while the map runs. Render lanes, the downloader and the UI
 * hand it jobs and wait for them; it runs the most urgent waiting job first. Files are read with few large
 * reads into PSRAM buffers, and a long read or write of lower priority stops between chunks when a more
 * urgent job arrives, so its owner can queue it again.
 */
class IoTask {
public:
    /** Small enough that the display, on the same SPI bus on the T-Deck, waits at most about 40 ms. */
    static constexpr size_t CHUNK_BYTES = 16 * 1024;

    bool start();
    void stop();

    /** Runs a job on the I/O task and waits for its result. Returns false right away when the task is not running. */
    bool run(IoPriority priority, IoJob job, void* context);

    /** For jobs: whether the running job should stop because a more urgent one waits, or prefetch was cancelled. */
    bool shouldYield() const;
    /** For jobs: whether the running job stopped early through shouldYield(). Reset for each job. */
    bool yielded() const { return runningYielded; }
    void markYielded() { runningYielded = true; }
    IoPriority runningPriority() const { return running; }

    /** Makes the running and waiting prefetch reads stop at their next chunk, after input moved the view. */
    void cancelPrefetch() { prefetchGeneration++; }

    /** For jobs: reads a whole file into buffer in large chunks. Returns false on error or when it yielded. */
    bool readFile(const char* path, FileBuffer& buffer);
    /** For jobs: writes bytes to path through path.tmp and a rename, in large chunks. */
    bool writeFile(const char* path, const char* temporaryPath, const uint8_t* bytes, size_t size, bool canYield);

    /** Statistics for the log: jobs run, and the longest wait of a visible job. */
    uint32_t longestVisibleWaitMs() const { return longestVisibleWait; }
    /** The fewest bytes of stack the task had left so far. */
    TaskHandle_t taskHandle() const { return task; }
    uint32_t stackLeft() const { return task != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(task)) : 0; }

private:
    struct Request {
        IoJob job;
        void* context;
        SemaphoreHandle_t done;
        bool result;
        uint32_t queuedAt;
        uint32_t prefetchGeneration;
    };

    static void taskMain(void* context);
    void loop();

    TaskHandle_t task = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t wake = nullptr;
    SemaphoreHandle_t stopped = nullptr;
    std::atomic<bool> stopRequested = false;
    std::atomic<bool> runningTask = false;
    IoQueue<Request*, 16> queue;
    IoPriority running = IoPriority::Background;
    uint32_t runningGeneration = 0;
    bool runningYielded = false;
    std::atomic<uint32_t> prefetchGeneration = 0;
    uint32_t longestVisibleWait = 0;
};

/** Runs a lambda on the I/O task and waits for it, for the UI's occasional file access. */
template <typename Function>
bool runOnCard(IoTask& io, Function& function, IoPriority priority = IoPriority::Visible) {
    return io.run(priority, [](IoTask&, void* context) { return (*static_cast<Function*>(context))(); }, &function);
}

} // namespace maps
