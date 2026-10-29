// SPDX-License-Identifier: GPL-3.0-or-later
#include "IoTask.h"

#include "../platform/Platform.h"

#include <cstdio>
#include <initializer_list>

namespace maps {

namespace {

/** Jobs run on this stack: folder walks, meta.json text (1 KB) parsed with sscanf, FatFs long names, paths. */
constexpr uint32_t TASK_STACK = 8192;
/**
 * The same as the app task and the render lanes. The SD card driver keeps the CPU busy while it transfers,
 * and on the T-Deck the display shares the SPI bus, so a higher priority starved drawing (frames of 1-3 s).
 */
constexpr UBaseType_t TASK_PRIORITY = 4;
/** Longer jobs are logged, they hold up everything behind them. */
constexpr uint32_t SLOW_JOB_MS = 300;

} // namespace

bool IoTask::start() {
    mutex = xSemaphoreCreateMutex();
    wake = xSemaphoreCreateCounting(64, 0);
    stopped = xSemaphoreCreateBinary();
    if (mutex == nullptr || wake == nullptr || stopped == nullptr) {
        return false;
    }
    stopRequested = false;
    // Not pinned, so it never takes a core from one render lane only
    if (xTaskCreatePinnedToCore(taskMain, "maps-io", TASK_STACK, this, TASK_PRIORITY, &task, tskNO_AFFINITY) != pdPASS) {
        task = nullptr;
        return false;
    }
    runningTask = true;
    // Job addresses in the log are matched against the ELF's symbols through this one
    logInfo("I/O task started, taskMain at %p", reinterpret_cast<void*>(&IoTask::taskMain));
    return true;
}

void IoTask::stop() {
    if (task != nullptr) {
        stopRequested = true;
        xSemaphoreGive(wake);
        xSemaphoreTake(stopped, portMAX_DELAY);
        task = nullptr;
    }
    runningTask = false;
    for (SemaphoreHandle_t* handle : { &mutex, &wake, &stopped }) {
        if (*handle != nullptr) {
            vSemaphoreDelete(*handle);
            *handle = nullptr;
        }
    }
}

void IoTask::taskMain(void* context) {
    auto* io = static_cast<IoTask*>(context);
    io->loop();
    xSemaphoreGive(io->stopped);
    vTaskDelete(nullptr);
}

bool IoTask::run(IoPriority priority, IoJob job, void* context) {
    if (!runningTask) {
        return false;
    }
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    if (done == nullptr) {
        return false;
    }
    Request request { job, context, done, false, nowMs(), prefetchGeneration.load() };
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool queued = queue.push(&request, priority);
    xSemaphoreGive(mutex);
    if (!queued) {
        vSemaphoreDelete(done);
        logError("I/O queue full");
        return false;
    }
    xSemaphoreGive(wake);
    xSemaphoreTake(done, portMAX_DELAY);
    vSemaphoreDelete(done);
    return request.result;
}

bool IoTask::shouldYield() const {
    if (running == IoPriority::Visible) {
        return false;
    }
    if (running == IoPriority::Prefetch && runningGeneration != prefetchGeneration.load()) {
        return true;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool urgent = queue.hasMoreUrgent(running);
    xSemaphoreGive(mutex);
    return urgent;
}

void IoTask::loop() {
    while (true) {
        xSemaphoreTake(wake, portMAX_DELAY);
        while (true) {
            Request* request = nullptr;
            IoPriority priority = IoPriority::Background;
            xSemaphoreTake(mutex, portMAX_DELAY);
            queue.pop(request, &priority);
            xSemaphoreGive(mutex);
            if (request == nullptr) {
                break;
            }
            if (stopRequested) {
                request->result = false;
            } else {
                running = priority;
                runningGeneration = request->prefetchGeneration;
                runningYielded = false;
                if (priority == IoPriority::Visible) {
                    const uint32_t waited = nowMs() - request->queuedAt;
                    longestVisibleWait = waited > longestVisibleWait ? waited : longestVisibleWait;
                }
                // A prefetch cancelled while it waited does not start
                if (priority == IoPriority::Prefetch && runningGeneration != prefetchGeneration.load()) {
                    runningYielded = true;
                    request->result = false;
                } else {
                    const uint32_t started = nowMs();
                    request->result = request->job(*this, request->context);
                    const uint32_t took = nowMs() - started;
                    if (took > SLOW_JOB_MS) {
                        logInfo("Slow I/O job %u ms (priority %d, waited %u ms, job %p)", took, static_cast<int>(priority), started - request->queuedAt,
                            reinterpret_cast<void*>(request->job));
                    }
                }
                running = IoPriority::Background;
            }
            xSemaphoreGive(request->done);
        }
        if (stopRequested) {
            break;
        }
    }
}

bool IoTask::readFile(const char* path, FileBuffer& buffer) {
    buffer.clear();
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    bool ok = fseek(file, 0, SEEK_END) == 0;
    const long size = ok ? ftell(file) : -1;
    ok = ok && size > 0 && fseek(file, 0, SEEK_SET) == 0 && buffer.reserve(static_cast<size_t>(size));
    size_t done = 0;
    while (ok && done < static_cast<size_t>(size)) {
        if (done > 0 && shouldYield()) {
            markYielded();
            ok = false;
            break;
        }
        if (done > 0) {
            // Between chunks the display gets the shared SPI bus
            vTaskDelay(1);
        }
        const size_t wanted = static_cast<size_t>(size) - done < CHUNK_BYTES ? static_cast<size_t>(size) - done : CHUNK_BYTES;
        const size_t count = fread(buffer.writable() + done, 1, wanted, file);
        ok = count == wanted;
        done += count;
    }
    fclose(file);
    if (ok) {
        buffer.setSize(done);
    }
    return ok;
}

bool IoTask::writeFile(const char* path, const char* temporaryPath, const uint8_t* bytes, size_t size, bool canYield) {
    FILE* file = fopen(temporaryPath, "wb");
    if (file == nullptr) {
        return false;
    }
    bool ok = true;
    size_t done = 0;
    while (ok && done < size) {
        if (canYield && done > 0 && shouldYield()) {
            markYielded();
            ok = false;
            break;
        }
        if (done > 0) {
            vTaskDelay(1);
        }
        const size_t count = size - done < CHUNK_BYTES ? size - done : CHUNK_BYTES;
        ok = fwrite(bytes + done, 1, count, file) == count;
        done += count;
    }
    ok = fclose(file) == 0 && ok;
    // Most files are new: the old one is only removed when the rename finds it, each FAT operation holds the card
    if (ok && rename(temporaryPath, path) != 0) {
        remove(path);
        ok = rename(temporaryPath, path) == 0;
    }
    if (!ok) {
        remove(temporaryPath);
    }
    return ok;
}

} // namespace maps
