// SPDX-License-Identifier: GPL-3.0-or-later
#include "LabelFontMaker.h"

#include "../mvt/ByteInput.h"
#include "../platform/Platform.h"

#include <binfont/generator.h>
#include <freertos/idf_additions.h>
#include <tactility/memory.h>

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

namespace maps {

namespace {

/** The stb_truetype rasteriser in the generator keeps its work areas on the stack. */
constexpr uint32_t TASK_STACK = 12288;
constexpr UBaseType_t TASK_PRIORITY = 1;
/** When stop() logs that a font in progress holds it up. */
constexpr int STOP_WAIT_MS = 6000;

struct ReadJob {
    const char* path;
    FileBuffer* buffer;
    bool yielded;
};

bool readJob(IoTask& io, void* context) {
    auto* job = static_cast<ReadJob*>(context);
    const bool ok = io.readFile(job->path, *job->buffer);
    job->yielded = io.yielded();
    return ok;
}

struct WriteJob {
    const char* folder;
    const char* path;
    const uint8_t* bytes;
    size_t size;
};

bool writeJob(IoTask& io, void* context) {
    auto* job = static_cast<WriteJob*>(context);
    // The folder is made on first use; it exists already afterwards
    mkdir(job->folder, 0777);
    char temporary[200];
    snprintf(temporary, sizeof(temporary), "%s.tmp", job->path);
    return io.writeFile(job->path, temporary, job->bytes, job->size, false);
}

} // namespace

void LabelFontMaker::cachePath(const char* root, int i, char* out, size_t size) {
    snprintf(out, size, "%s/fonts/lbl%d%c.bin", root, EM_SIZES[i], FONT_VERSION);
}

namespace {

struct RemoveOldJob {
    const char* root;
    int emSize;
};

/** Deletes the cached sizes an older font version made: the cache keeps one copy of each glyph. */
bool removeOldJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<RemoveOldJob*>(context);
    for (char version = 'a'; version < LabelFontMaker::FONT_VERSION; version++) {
        char path[200];
        snprintf(path, sizeof(path), "%s/fonts/lbl%d%c.bin", job->root, job->emSize, version);
        remove(path);
    }
    return true;
}

} // namespace

void LabelFontMaker::configure(IoTask& ioTask, const char* root, const char* ttf) {
    io = &ioTask;
    snprintf(rootPath, sizeof(rootPath), "%s", root);
    snprintf(ttfPath, sizeof(ttfPath), "%s", ttf);
}

bool LabelFontMaker::start(unsigned missingMask) {
    if (io == nullptr || task != nullptr || missingMask == 0) {
        return false;
    }
    wanted = missingMask;
    finished = false;
    stopRequested = false;
    written = 0;
    if (xTaskCreatePinnedToCoreWithCaps(taskMain, "maps-fonts", TASK_STACK, this, TASK_PRIORITY, &task, tskNO_AFFINITY,
            MALLOC_CAP_SPIRAM) != pdPASS) {
        task = nullptr;
        logError("Label font task not started, the map keeps the system fonts");
        return false;
    }
    return true;
}

void LabelFontMaker::taskMain(void* context) {
    auto* maker = static_cast<LabelFontMaker*>(context);
    maker->run();
    maker->finished = true;
    // Deleted by stop(), a task cannot free its own PSRAM stack
    vTaskSuspend(nullptr);
}

void LabelFontMaker::run() {
    const uint32_t start = nowMs();
    FileBuffer ttf;
    ReadJob read { ttfPath, &ttf, false };
    bool ok = false;
    // A background read gives way to the map's tile reads; while the map is busy it is simply tried again
    for (int attempt = 0; attempt < 600 && !stopRequested; attempt++) {
        ok = io->run(IoPriority::Background, readJob, &read);
        if (ok || !read.yielded) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!ok || ttf.size() == 0) {
        logError("Label font: cannot read %s, the map keeps the system fonts", ttfPath);
        return;
    }
    char folder[180];
    snprintf(folder, sizeof(folder), "%s/fonts", rootPath);
    for (int i = 0; i < COUNT && !stopRequested; i++) {
        if ((wanted & (1u << i)) == 0) {
            continue;
        }
        const uint32_t fontStart = nowMs();
        BinFontGeneratorConfig config {};
        config.ttf_data = ttf.bytes();
        config.ttf_size = ttf.size();
        config.size = static_cast<uint16_t>(EM_SIZES[i]);
        config.bpp = 4;
        uint8_t* data = nullptr;
        size_t size = 0;
        const error_t error = binfont_generate(&config, &data, &size);
        if (error != ERROR_NONE || data == nullptr) {
            logError("Label font %d px: generator failed with error %d, the map keeps the system font", EM_SIZES[i], static_cast<int>(error));
            memory_free(data);
            continue;
        }
        const uint32_t generated = nowMs();
        char path[200];
        cachePath(rootPath, i, path, sizeof(path));
        WriteJob write { folder, path, data, size };
        const bool ok = io->run(IoPriority::Background, writeJob, &write);
        memory_free(data);
        if (ok) {
            written = written.load() | (1u << i);
            RemoveOldJob old { rootPath, EM_SIZES[i] };
            io->run(IoPriority::Background, removeOldJob, &old);
        }
        logInfo("Label font %d px: %u bytes, generated in %u ms, %s in %u ms", EM_SIZES[i], static_cast<unsigned>(size),
            static_cast<unsigned>(generated - fontStart), ok ? "written" : "NOT written", static_cast<unsigned>(nowMs() - generated));
        // Gives the render lanes and the UI their turn between two fonts
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    logInfo("Label fonts made in %u ms in total", static_cast<unsigned>(nowMs() - start));
}

void LabelFontMaker::stop() {
    if (task == nullptr) {
        return;
    }
    stopRequested = true;
    // A font being made cannot be interrupted, and the task still uses this object and the I/O task: wait for it
    for (int waited = 0; !finished.load(); waited += 10) {
        if (waited == STOP_WAIT_MS) {
            logError("Label font task still busy after %d ms, waiting", STOP_WAIT_MS);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDeleteWithCaps(task);
    task = nullptr;
}

} // namespace maps
