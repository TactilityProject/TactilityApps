// SPDX-License-Identifier: GPL-3.0-or-later
#include "GpxImport.h"

#include "../platform/Platform.h"
#include "../render/PreparedTile.h"

#include <freertos/idf_additions.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>
#include <new>

namespace maps {

namespace {

constexpr uint32_t TASK_STACK = 6144;
constexpr UBaseType_t TASK_PRIORITY = 2;
constexpr size_t CHUNK = 8192;

struct ChunkJob {
    const char* path;
    long offset;
    char* buffer;
    size_t count;
    long fileSize;
};

bool readChunkJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<ChunkJob*>(context);
    FILE* file = fopen(job->path, "rb");
    if (file == nullptr) {
        return false;
    }
    bool ok = true;
    if (job->fileSize < 0) {
        ok = fseek(file, 0, SEEK_END) == 0;
        job->fileSize = ok ? ftell(file) : -1;
    }
    ok = ok && fseek(file, job->offset, SEEK_SET) == 0;
    job->count = ok ? fread(job->buffer, 1, CHUNK, file) : 0;
    fclose(file);
    return ok;
}

struct CacheJob {
    char path[PATH_MAX_LENGTH + 8];
    FileBuffer* buffer;
    const ByteWriter* data;
};

bool readCacheJob(IoTask& io, void* context) {
    auto* job = static_cast<CacheJob*>(context);
    return io.readFile(job->path, *job->buffer);
}

bool writeCacheJob(IoTask& io, void* context) {
    auto* job = static_cast<CacheJob*>(context);
    char temporary[PATH_MAX_LENGTH + 16];
    snprintf(temporary, sizeof(temporary), "%s.tmp", job->path);
    return io.writeFile(job->path, temporary, job->data->bytes(), job->data->size(), false);
}

} // namespace

GpxImport::~GpxImport() {
    // The map ends the app only when no import runs, see MapView::end()
    if (layer != nullptr) {
        layer->~GpxLayer();
        largeFree(layer);
    }
}

bool GpxImport::start(IoTask& ioTask, const char* path) {
    if (currentState != State::Idle) {
        return false;
    }
    io = &ioTask;
    snprintf(filePath, sizeof(filePath), "%s", path);
    message[0] = '\0';
    currentState = State::Busy;
    if (xTaskCreatePinnedToCoreWithCaps(taskMain, "maps-gpx", TASK_STACK, this, TASK_PRIORITY, &task, 1, MALLOC_CAP_SPIRAM) != pdPASS) {
        currentState = State::Idle;
        return false;
    }
    return true;
}

void GpxImport::taskMain(void* context) {
    auto* import = static_cast<GpxImport*>(context);
    import->run();
    vTaskSuspend(nullptr);
}

void GpxImport::run() {
    void* memory = largeAlloc(sizeof(GpxLayer));
    layer = memory != nullptr ? new (memory) GpxLayer() : nullptr;
    if (layer == nullptr) {
        snprintf(message, sizeof(message), "Not enough memory");
        currentState = State::Failed;
        return;
    }
    // The cache sits next to the file: walk.gpx -> walk.gpc
    CacheJob cache {};
    snprintf(cache.path, sizeof(cache.path), "%s", filePath);
    const size_t length = strlen(cache.path);
    if (length > 4) {
        cache.path[length - 1] = 'c';
    }
    char* buffer = static_cast<char*>(largeAlloc(CHUNK));
    if (buffer == nullptr) {
        snprintf(message, sizeof(message), "Not enough memory");
        currentState = State::Failed;
        return;
    }
    ChunkJob chunk { filePath, 0, buffer, 0, -1 };
    if (!io->run(IoPriority::Background, readChunkJob, &chunk) || chunk.fileSize < 0) {
        largeFree(buffer);
        snprintf(message, sizeof(message), "Cannot read the file");
        currentState = State::Failed;
        return;
    }
    const auto sourceSize = static_cast<uint32_t>(chunk.fileSize);
    FileBuffer cached;
    cache.buffer = &cached;
    if (io->run(IoPriority::Background, readCacheJob, &cache) && layer->load(cached.bytes(), cached.size(), sourceSize)) {
        largeFree(buffer);
        logInfo("GPX %s from its cache: %d lines, %d waypoints", filePath, layer->lineCount(), layer->pinCount());
        currentState = State::Done;
        return;
    }
    // No current cache: the file is read once, in chunks, with the card free for the map in between
    void* builderMemory = largeAlloc(sizeof(GpxCacheBuilder));
    auto* builder = builderMemory != nullptr ? new (builderMemory) GpxCacheBuilder() : nullptr;
    bool ok = builder != nullptr;
    const uint32_t start = nowMs();
    while (ok && chunk.count > 0) {
        ok = builder->feed(buffer, chunk.count);
        chunk.offset += static_cast<long>(chunk.count);
        if (ok && chunk.offset < chunk.fileSize) {
            ok = io->run(IoPriority::Background, readChunkJob, &chunk);
        } else {
            chunk.count = 0;
        }
        vTaskDelay(1);
    }
    largeFree(buffer);
    ByteWriter data;
    const bool built = builder != nullptr && builder->finish(sourceSize, data);
    if (!built) {
        snprintf(message, sizeof(message), "%s", builder == nullptr ? "Not enough memory" : builder->error());
    } else {
        logInfo("GPX %s read in %u ms: %d of %d points kept", filePath, nowMs() - start, builder->pointsKept(), builder->pointsRead());
    }
    if (builder != nullptr) {
        builder->~GpxCacheBuilder();
        largeFree(builder);
    }
    if (!built || !layer->load(data.bytes(), data.size(), sourceSize)) {
        if (message[0] == '\0') {
            snprintf(message, sizeof(message), "Not a usable GPX file");
        }
        currentState = State::Failed;
        return;
    }
    cache.data = &data;
    if (!io->run(IoPriority::Background, writeCacheJob, &cache)) {
        // Only slower next time
        logError("Could not write %s", cache.path);
    }
    currentState = State::Done;
}

GpxLayer* GpxImport::takeLayer() {
    GpxLayer* result = currentState == State::Done ? layer : nullptr;
    if (result != nullptr) {
        layer = nullptr;
    }
    acknowledge();
    return result;
}

void GpxImport::acknowledge() {
    if (currentState == State::Done || currentState == State::Failed) {
        if (task != nullptr) {
            // The state is set just before the task suspends itself
            TaskStatus_t status {};
            for (int i = 0; i < 100; i++) {
                vTaskGetInfo(task, &status, pdFALSE, eInvalid);
                if (status.eCurrentState == eSuspended) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(5));
            }
            vTaskDeleteWithCaps(task);
            task = nullptr;
        }
        if (layer != nullptr) {
            layer->~GpxLayer();
            largeFree(layer);
            layer = nullptr;
        }
        currentState = State::Idle;
    }
}

} // namespace maps
