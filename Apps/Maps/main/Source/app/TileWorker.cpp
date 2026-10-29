// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileWorker.h"

#include "../platform/Platform.h"
#include "../render/TileCodec.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

namespace maps {

namespace {

/** Rendering, and conversions, where ROM tdefl alone needs about 3-4 KB of stack. */
/** tdefl, which deflates prepared tiles, keeps about 6 KB of Huffman tables on the stack: 8 KB left 600 bytes. */
constexpr uint32_t TASK_STACK = 10240;
constexpr UBaseType_t TASK_PRIORITY = 4;
constexpr uint32_t IDLE_WAIT_MS = 30;
constexpr char CACHE_MAGIC[4] = { 'T', 'M', 'C', '2' };
constexpr size_t CACHE_HEADER_BYTES = 12;
constexpr size_t TILE_PIXELS = static_cast<size_t>(TILE_SIZE) * TILE_SIZE;
constexpr size_t ENCODE_CAPACITY = TILE_PIXELS + 256;
/** Enough entries for the 200 MB cap: a dense rendered tile codes to about 96 KB. */
constexpr int CACHE_INDEX_ENTRIES = 6000;
constexpr int SAVE_INDEX_EVERY_WRITES = 25;

/** Checks a rendered tile file from the SD card cache. Returns the number of coded words, 0 when unusable. */
uint32_t checkCacheFile(const uint8_t* bytes, size_t size, int& sourceZoom) {
    uint32_t version = 0;
    uint32_t words = 0;
    if (size < CACHE_HEADER_BYTES || memcmp(bytes, CACHE_MAGIC, 4) != 0) {
        return 0;
    }
    memcpy(&version, bytes + 4, 4);
    memcpy(&words, bytes + 8, 4);
    // The top byte of the version holds the zoom level of the vector tile it was drawn from
    sourceZoom = static_cast<int>(version >> 24);
    if ((version & 0xFFFFFF) != STYLE_VERSION || size != CACHE_HEADER_BYTES + static_cast<size_t>(words) * 2) {
        return 0;
    }
    return words;
}

struct WriteCacheJob {
    const char* root;
    const char* style;
    TileKey tile;
    const uint8_t* bytes;
    size_t size;
};

bool writeCacheJob(IoTask& io, void* context) {
    auto* job = static_cast<WriteCacheJob*>(context);
    char path[PATH_MAX_LENGTH];
    char folder[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH];
    snprintf(folder, sizeof(folder), "%s/cache", job->root);
    mkdir(folder, 0777);
    snprintf(folder, sizeof(folder), "%s/cache/%s", job->root, job->style);
    mkdir(folder, 0777);
    snprintf(folder, sizeof(folder), "%s/cache/%s/%d", job->root, job->style, job->tile.z);
    mkdir(folder, 0777);
    if (!cacheFolderPath(folder, sizeof(folder), job->root, job->style, job->tile) ||
        !cachePath(path, sizeof(path), job->root, job->style, job->tile)) {
        return false;
    }
    mkdir(folder, 0777);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    return io.writeFile(path, temporary, job->bytes, job->size, true);
}

struct IndexFileJob {
    const char* root;
    const char* style;
    FileBuffer* buffer;
    const ByteWriter* bytes;
};

bool readIndexJob(IoTask& io, void* context) {
    auto* job = static_cast<IndexFileJob*>(context);
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/cache/%s/index.bin", job->root, job->style);
    return io.readFile(path, *job->buffer);
}

bool writeIndexJob(IoTask& io, void* context) {
    auto* job = static_cast<IndexFileJob*>(context);
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/cache", job->root);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/cache/%s", job->root, job->style);
    mkdir(path, 0777);
    snprintf(path, sizeof(path), "%s/cache/%s/index.bin", job->root, job->style);
    snprintf(temporary, sizeof(temporary), "%s/cache/%s/index.tmp", job->root, job->style);
    return io.writeFile(path, temporary, job->bytes->bytes(), job->bytes->size(), false);
}

struct EvictJob {
    const char* root;
    const char* style;
    TileKey tile;
};

bool evictJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<EvictJob*>(context);
    char path[PATH_MAX_LENGTH];
    // Only ever a file of the cache folder, never anything under maps/
    if (!cachePath(path, sizeof(path), job->root, job->style, job->tile) || !isInsideCache(path, job->root)) {
        return false;
    }
    removeFile(path);
    return true;
}

struct WalkJob {
    CacheWalker* walker;
    CacheIndex* index;
    const CacheWalker::IndexLock* lock;
    bool more;
};

bool walkJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<WalkJob*>(context);
    job->more = job->walker->step(*job->index, job->lock);
    return true;
}

struct ConvertReadJob {
    const char* root;
    const char* region;
    TileKey tile;
    FileBuffer* buffer;
    uint32_t size;
    bool current;
    bool yielded;
};

bool convertReadJob(IoTask& io, void* context) {
    auto* job = static_cast<ConvertReadJob*>(context);
    char path[PATH_MAX_LENGTH];
    char prepared[PATH_MAX_LENGTH];
    if (!tilePath(path, sizeof(path), job->root, job->region, job->tile) || !fileSize(path, job->size) ||
        !preparedPath(prepared, sizeof(prepared), job->root, job->region, job->tile)) {
        return false;
    }
    // Queued twice, or converted by the other lane meanwhile
    job->current = PreparedScanner::isCurrent(prepared, job->tile, job->size);
    const bool ok = job->current || io.readFile(path, *job->buffer);
    job->yielded = io.yielded();
    return ok;
}

struct ConvertWriteJob {
    const char* root;
    const char* region;
    TileKey tile;
    const ByteWriter* bytes;
    bool yielded;
};

bool convertWriteJob(IoTask& io, void* context) {
    auto* job = static_cast<ConvertWriteJob*>(context);
    char prepared[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH];
    const bool ok = preparedPath(prepared, sizeof(prepared), job->root, job->region, job->tile) &&
        preparedTempPath(temporary, sizeof(temporary), job->root, job->region, job->tile) &&
        io.writeFile(prepared, temporary, job->bytes->bytes(), job->bytes->size(), true);
    job->yielded = io.yielded();
    return ok;
}

struct ScanJob {
    PreparedScanner* scanner;
    const TileStore* store;
    PreparedScanner::StaleCallback callback;
    void* result;
    bool more;
};

bool scanJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<ScanJob*>(context);
    job->more = job->scanner->step(*job->store, job->callback, job->result);
    return true;
}

} // namespace

bool TileWorker::start(TileStore* tileStore, IoTask* ioTask, const Palette* tilePalette, int cacheSlots, const char* cacheRoot) {
    store = tileStore;
    io = ioTask;
    palette = tilePalette;
    snprintf(root, sizeof(root), "%s", cacheRoot);
    mutex = xSemaphoreCreateMutex();
    stopped = xSemaphoreCreateCounting(LANES, 0);
    if (mutex == nullptr || stopped == nullptr || tileCache.init(cacheSlots) < 4) {
        logError("Tile worker: out of memory");
        return false;
    }
    logInfo("Tile cache: %d slots of %d KB", tileCache.slotCount(), TileCache::TILE_BYTES / 1024);
    styleCacheId(palette == &monochromePalette(), styleId, sizeof(styleId));
    if (!cacheIndex.init(CACHE_INDEX_ENTRIES)) {
        logError("Tile worker: no memory for the cache index");
        return false;
    }
    // The index of the SD card cache, rebuilt from the files in idle time when it is missing
    FileBuffer indexFile;
    IndexFileJob read { root, styleId, &indexFile, nullptr };
    if (io->run(IoPriority::Visible, readIndexJob, &read) && cacheIndex.load(indexFile.bytes(), indexFile.size())) {
        logInfo("SD cache %s (%s): %d tiles, %u KB", styleId, sdCache ? "on" : "off", cacheIndex.size(),
            static_cast<unsigned>(cacheIndex.totalBytes() / 1024));
    } else {
        logInfo("SD cache %s (%s): no index yet", styleId, sdCache ? "on" : "off");
    }
    // With the SD card cache off its files go too, they only take space
    cacheWalker.begin(root, styleId, sdCache);
    if (!sdCache) {
        cacheIndex.clear();
    }
    stopRequested = false;
    laneCount = 0;
    for (int i = 0; i < LANES; i++) {
        Lane& lane = lanes[i];
        lane.worker = this;
        lane.index = i;
        lane.active = false;
        lane.converting = false;
        lane.prefetching = false;
        lane.cancel = false;
        lane.renderer.setCancelFlag(&lane.cancel);
        lane.writeBuffer = static_cast<uint16_t*>(largeAlloc(TileCache::TILE_BYTES));
        lane.encodeBuffer = static_cast<uint16_t*>(largeAlloc(ENCODE_CAPACITY * sizeof(uint16_t) + CACHE_HEADER_BYTES));
        if (lane.writeBuffer == nullptr || lane.encodeBuffer == nullptr || !lane.renderer.init()) {
            logError("Tile worker: no memory for render lane %d", i);
            break;
        }
        char name[16];
        snprintf(name, sizeof(name), "maps-render%d", i);
        if (xTaskCreatePinnedToCore(taskMain, name, TASK_STACK, &lane, TASK_PRIORITY, &lane.task, i) != pdPASS) {
            logError("Tile worker: task %d not started", i);
            break;
        }
        laneCount++;
    }
    logMemory("render lanes started");
    return laneCount > 0;
}

void TileWorker::stop() {
    stopRequested = true;
    for (int i = 0; i < laneCount; i++) {
        xSemaphoreTake(stopped, portMAX_DELAY);
    }
    if (laneCount > 0) {
        saveCacheIndex();
    }
    for (int i = 0; i < LANES; i++) {
        largeFree(lanes[i].writeBuffer);
        lanes[i].writeBuffer = nullptr;
        largeFree(lanes[i].encodeBuffer);
        lanes[i].encodeBuffer = nullptr;
    }
    laneCount = 0;
    if (stopped != nullptr) {
        vSemaphoreDelete(stopped);
        stopped = nullptr;
    }
    if (mutex != nullptr) {
        vSemaphoreDelete(mutex);
        mutex = nullptr;
    }
}

void TileWorker::taskMain(void* context) {
    auto* lane = static_cast<Lane*>(context);
    TileWorker* worker = lane->worker;
    worker->loop(*lane);
    xSemaphoreGive(worker->stopped);
    vTaskDelete(nullptr);
}

void TileWorker::want(const TileKey* tiles, int count, int visibleCount) {
    lock();
    wantedCount = count < MAX_WANTED ? count : MAX_WANTED;
    visibleWanted = visibleCount < wantedCount ? visibleCount : wantedCount;
    memcpy(wanted, tiles, sizeof(TileKey) * wantedCount);
    unlock();
}

bool TileWorker::isSettled(const TileKey& tile) const {
    if (tileCache.contains(tile)) {
        return true;
    }
    for (int i = 0; i < missingCount; i++) {
        if (missing[i] == tile) {
            return true;
        }
    }
    return false;
}

void TileWorker::tilesArrived() {
    lock();
    missingCount = 0;
    missingNext = 0;
    tileCache.dropMagnified();
    unlock();
    // New originals get their prepared copies in idle time
    rescanRequested = true;
    changed = true;
}

bool TileWorker::queueConversion(const TileKey& tile, int region) {
    for (int i = 0; i < conversionCount; i++) {
        if (conversions[i].tile == tile) {
            return true;
        }
    }
    for (int i = 0; i < laneCount; i++) {
        if (lanes[i].converting && lanes[i].convertingTile == tile) {
            return true;
        }
    }
    if (conversionCount >= MAX_CONVERSIONS) {
        return false;
    }
    conversions[conversionCount++] = { tile, region };
    return true;
}

bool TileWorker::collectFromScan(const TileKey& tile, int region, void* context) {
    auto* found = static_cast<ScanResult*>(context);
    if (found->count >= found->room) {
        return false;
    }
    found->items[found->count++] = { tile, region };
    return true;
}

bool TileWorker::convertOne(Lane& lane) {
    lock();
    Conversion job {};
    bool found = false;
    bool otherConverting = false;
    for (int i = 0; i < laneCount; i++) {
        otherConverting = otherConverting || lanes[i].converting;
    }
    if (conversionCount > 0 && !otherConverting) {
        // One conversion at a time: each needs about 0.4 MB of PSRAM, which a download needs as well.
        // The oldest first, so the tiles just drawn from originals come before the scanned ones
        job = conversions[0];
        for (int i = 1; i < conversionCount; i++) {
            conversions[i - 1] = conversions[i];
        }
        conversionCount--;
        found = true;
        lane.converting = true;
        lane.convertingTile = job.tile;
    } else if ((rescanRequested || !scanner.isComplete()) && !scanning) {
        // Only one lane walks the folders at a time, and only that lane touches the scanner
        scanning = true;
    } else {
        unlock();
        return false;
    }
    char region[REGION_NAME_MAX + 1] = "";
    char version[48] = "";
    if (found && job.region < store->regionCount()) {
        snprintf(region, sizeof(region), "%s", store->regionName(job.region));
        snprintf(version, sizeof(version), "%s", store->regionVersion(job.region));
    }
    unlock();

    if (!found) {
        if (rescanRequested.exchange(false)) {
            scanner.restart();
        }
        // Kept in the worker, not on the lane's small stack, only the scanning lane uses it
        ScanResult& result = scanResult;
        result.count = 0;
        lock();
        result.room = MAX_CONVERSIONS - conversionCount;
        unlock();
        ScanJob scan { &scanner, store, collectFromScan, &result, false };
        io->run(IoPriority::Background, scanJob, &scan);
        lock();
        for (int i = 0; i < result.count; i++) {
            queueConversion(result.items[i].tile, result.items[i].region);
        }
        scanning = false;
        unlock();
        return scan.more || result.count > 0;
    }

    const uint32_t start = nowMs();
    ConvertReadJob read { root, region, job.tile, &lane.fileBuffer, 0, false, false };
    bool ok = region[0] != '\0' && io->run(IoPriority::Background, convertReadJob, &read);
    const uint32_t loaded = nowMs();
    bool requeue = read.yielded;
    if (ok && !read.current) {
        ConvertStats stats {};
        MemoryInput input(lane.fileBuffer.bytes(), lane.fileBuffer.size());
        ok = lane.converter.convert(lane.renderer.tileDecoder(), &input, job.tile, read.size, version, deflatePrepared, lane.converted, &stats);
        const uint32_t convertedAt = nowMs();
        ConvertWriteJob write { root, region, job.tile, &lane.converted, false };
        ok = ok && io->run(IoPriority::Background, convertWriteJob, &write);
        requeue = write.yielded;
        if (ok) {
            logInfo("[%d] Prepared %d/%d/%d: %u -> %u bytes, read %u ms, convert %u ms, write %u ms", lane.index, job.tile.z, job.tile.x,
                job.tile.y, static_cast<unsigned>(read.size), static_cast<unsigned>(lane.converted.size()), loaded - start, convertedAt - loaded,
                nowMs() - convertedAt);
        }
    }
    // A read or write that gave way to tiles on screen is tried again later
    if (!ok && !requeue) {
        logError("[%d] Could not prepare %d/%d/%d", lane.index, job.tile.z, job.tile.x, job.tile.y);
    }
    lock();
    lane.converting = false;
    if (requeue) {
        queueConversion(job.tile, job.region);
    }
    unlock();
    // Something was done, or the card is busy with more urgent work
    return true;
}

void TileWorker::addGap(const TileKey& tile) {
    for (int i = 0; i < gapCount; i++) {
        if (gaps[i] == tile) {
            return;
        }
    }
    if (gapCount < MAX_GAPS) {
        gaps[gapCount++] = tile;
    }
}

int TileWorker::takeGaps(TileKey* out, int max) {
    lock();
    const int count = gapCount < max ? gapCount : max;
    for (int i = 0; i < count; i++) {
        out[i] = gaps[i];
    }
    // Whatever did not fit stays for the next call
    for (int i = count; i < gapCount; i++) {
        gaps[i - count] = gaps[i];
    }
    gapCount -= count;
    unlock();
    return count;
}

void TileWorker::cancelPrefetch() {
    idle = false;
    lock();
    for (int i = 0; i < laneCount; i++) {
        if (lanes[i].active && lanes[i].prefetching) {
            lanes[i].cancel = true;
        }
    }
    unlock();
    io->cancelPrefetch();
}

void TileWorker::lockIndex(void* context) {
    static_cast<TileWorker*>(context)->lock();
}

void TileWorker::unlockIndex(void* context) {
    static_cast<TileWorker*>(context)->unlock();
}

void TileWorker::saveCacheIndex() {
    lock();
    const bool dirty = cacheIndex.isDirty();
    if (dirty) {
        cacheIndex.save(indexBytes);
        cacheIndex.setClean();
    }
    writesSinceSave = 0;
    unlock();
    if (dirty) {
        IndexFileJob job { root, styleId, nullptr, &indexBytes };
        io->run(IoPriority::Background, writeIndexJob, &job);
    }
}

bool TileWorker::tidyCache() {
    lock();
    TileKey tile {};
    const bool evict = sdCache && cacheIndex.nextEviction(tile);
    if (evict) {
        cacheIndex.erase(tile);
    }
    const bool walk = !evict && !walking && !cacheWalker.isComplete();
    if (walk) {
        walking = true;
    }
    unlock();
    if (evict) {
        EvictJob job { root, styleId, tile };
        io->run(IoPriority::Background, evictJob, &job);
        return true;
    }
    if (walk) {
        const CacheWalker::IndexLock indexLock { lockIndex, unlockIndex, this };
        WalkJob job { &cacheWalker, &cacheIndex, &indexLock, false };
        io->run(IoPriority::Background, walkJob, &job);
        lock();
        walking = false;
        unlock();
        return true;
    }
    return false;
}

bool TileWorker::claimNext(Lane& lane, bool visible, TileKey& tile, uint16_t*& pixels) {
    lock();
    bool found = false;
    const int first = visible ? 0 : visibleWanted;
    const int last = visible ? visibleWanted : wantedCount;
    for (int i = first; i < last && !found; i++) {
        const TileKey& candidate = wanted[i];
        if (tileCache.contains(candidate)) {
            continue;
        }
        bool skip = false;
        for (int j = 0; j < missingCount && !skip; j++) {
            skip = missing[j] == candidate;
        }
        for (int j = 0; j < laneCount && !skip; j++) {
            skip = j != lane.index && lanes[j].active && lanes[j].current == candidate;
        }
        if (skip) {
            continue;
        }
        pixels = tileCache.reserve(candidate);
        if (pixels != nullptr) {
            tile = candidate;
            lane.current = candidate;
            lane.active = true;
            found = true;
        }
    }
    unlock();
    return found;
}

bool TileWorker::writeOne(Lane& lane) {
    if (!sdCache) {
        return false;
    }
    lock();
    TileKey tile {};
    int sourceZoom = 0;
    const uint16_t* pixels = nullptr;
    while (pendingWriteCount > 0 && pixels == nullptr) {
        pendingWriteCount--;
        tile = pendingWrites[pendingWriteCount];
        sourceZoom = pendingSourceZoom[pendingWriteCount];
        pixels = tileCache.peek(tile);
    }
    if (pixels != nullptr) {
        memcpy(lane.writeBuffer, pixels, TileCache::TILE_BYTES);
    }
    unlock();
    if (pixels == nullptr) {
        return false;
    }
    const uint32_t start = nowMs();
    // Header: magic, style version with the source zoom level on top, and the number of coded 16-bit words
    auto* bytes = reinterpret_cast<uint8_t*>(lane.encodeBuffer);
    const size_t words = encodeTile(lane.writeBuffer, TILE_PIXELS, reinterpret_cast<uint16_t*>(bytes + CACHE_HEADER_BYTES), ENCODE_CAPACITY);
    if (words == 0) {
        return true;
    }
    const uint32_t version = STYLE_VERSION | (static_cast<uint32_t>(sourceZoom) << 24);
    const auto wordCount = static_cast<uint32_t>(words);
    memcpy(bytes, CACHE_MAGIC, 4);
    memcpy(bytes + 4, &version, 4);
    memcpy(bytes + 8, &wordCount, 4);
    WriteCacheJob job { root, styleId, tile, bytes, CACHE_HEADER_BYTES + words * 2 };
    if (io->run(IoPriority::Background, writeCacheJob, &job)) {
        lock();
        cacheIndex.put(tile, static_cast<uint32_t>(job.size));
        const bool save = ++writesSinceSave >= SAVE_INDEX_EVERY_WRITES;
        unlock();
        logInfo("[%d] Tile %d/%d/%d written to the SD cache in %u ms", lane.index, tile.z, tile.x, tile.y, nowMs() - start);
        if (save) {
            saveCacheIndex();
        }
    } else {
        // It gave way to more urgent card work, or the card failed: it is written later or never
        lock();
        if (pendingWriteCount < MAX_PENDING_WRITES && tileCache.peek(tile) != nullptr) {
            pendingSourceZoom[pendingWriteCount] = sourceZoom;
            pendingWrites[pendingWriteCount++] = tile;
        }
        unlock();
    }
    return true;
}

void TileWorker::finish(Lane& lane, const TileKey& tile, bool success, int sourceZoom) {
    lock();
    if (success) {
        tileCache.commit(tile, sourceZoom);
    } else {
        tileCache.release(tile);
        missing[missingNext] = tile;
        missingNext = (missingNext + 1) % 64;
        if (missingCount < 64) {
            missingCount++;
        }
    }
    lane.active = false;
    unlock();
    changed = true;
}

bool TileWorker::loadJob(IoTask& io, void* context) {
    auto* job = static_cast<LoadJob*>(context);
    TileWorker* worker = job->worker;
    FileBuffer& buffer = job->lane->fileBuffer;
    job->found = Found::Nothing;
    job->yielded = false;
    const bool hasSource = worker->store->find(job->tile, job->source);
    char path[PATH_MAX_LENGTH];
    // Only tiles the index knows are read, a missing file costs a folder lookup on the card
    worker->lock();
    const bool indexed = job->tryCache && worker->sdCache && (worker->cacheIndex.contains(job->tile) || !worker->cacheIndex.isComplete());
    worker->unlock();
    if (indexed && cachePath(path, sizeof(path), worker->root, worker->styleId, job->tile) && io.readFile(path, buffer)) {
        // A tile drawn magnified from a lower zoom level is drawn again once its own data is on the card
        int sourceZoom = 0;
        if (checkCacheFile(buffer.bytes(), buffer.size(), sourceZoom) > 0 && (!hasSource || job->source.tile.z == sourceZoom)) {
            job->cachedSourceZoom = sourceZoom;
            job->found = Found::Cached;
            worker->lock();
            worker->cacheIndex.touch(job->tile);
            worker->unlock();
            return true;
        }
    }
    if (io.yielded()) {
        job->yielded = true;
        return false;
    }
    if (!hasSource) {
        return false;
    }
    PreparedHeader header {};
    if (!job->skipPrepared && preparedPath(path, sizeof(path), worker->root, worker->store->regionName(job->source.region), job->source.tile) &&
        io.readFile(path, buffer) && readPreparedHeader(buffer.bytes(), buffer.size(), header) &&
        isPreparedCurrent(header, job->source.tile, job->source.size)) {
        job->found = Found::Prepared;
        return true;
    }
    if (!io.yielded() && io.readFile(job->source.path, buffer)) {
        job->found = Found::Original;
        return true;
    }
    job->yielded = io.yielded();
    return false;
}

bool TileWorker::produce(Lane& lane, const TileKey& tile, uint16_t* pixels, bool visible) {
    const uint32_t start = nowMs();
    const IoPriority priority = visible ? IoPriority::Visible : IoPriority::Prefetch;
    LoadJob job {};
    job.worker = this;
    job.lane = &lane;
    job.tile = tile;
    job.tryCache = true;
    io->run(priority, loadJob, &job);
    if (job.yielded) {
        return false;
    }
    const uint32_t loaded = nowMs();
    // A tile on screen without data of its own zoom level is a gap that browsing can fill
    if (visible) {
        const int bestZoom = tile.z < MAX_SOURCE_ZOOM ? tile.z : MAX_SOURCE_ZOOM;
        int haveZoom = -1;
        if (job.found == Found::Cached) {
            haveZoom = job.cachedSourceZoom;
        } else if (job.found != Found::Nothing) {
            haveZoom = job.source.tile.z;
        }
        if (haveZoom < bestZoom) {
            lock();
            addGap(ancestorOf(tile, bestZoom));
            unlock();
        }
    }
    if (job.found == Found::Cached) {
        int sourceZoom = 0;
        const uint32_t words = checkCacheFile(lane.fileBuffer.bytes(), lane.fileBuffer.size(), sourceZoom);
        const bool decoded = decodeTile(reinterpret_cast<const uint16_t*>(lane.fileBuffer.bytes() + CACHE_HEADER_BYTES), words, pixels, TILE_PIXELS);
        finish(lane, tile, decoded, sourceZoom);
        if (decoded) {
            logInfo("[%d] Tile %d/%d/%d from SD cache: read %u ms, decode %u ms", lane.index, tile.z, tile.x, tile.y, loaded - start, nowMs() - loaded);
        }
        return true;
    }
    RenderStats stats {};
    bool rendered = false;
    if (job.found == Found::Prepared) {
        rendered = lane.renderer.renderPrepared(lane.fileBuffer.bytes(), lane.fileBuffer.size(), tile, *palette, pixels, &stats);
        if (lane.renderer.wasCancelled()) {
            return false;
        }
        if (!rendered) {
            // A damaged prepared tile: the original is drawn and the copy made again
            job.tryCache = false;
            job.skipPrepared = true;
            io->run(priority, loadJob, &job);
            if (job.yielded) {
                return false;
            }
        }
    }
    if (job.found == Found::Original) {
        MemoryInput input(lane.fileBuffer.bytes(), lane.fileBuffer.size());
        rendered = lane.renderer.render(&input, job.source.tile, tile, *palette, pixels, &stats);
        if (lane.renderer.wasCancelled()) {
            return false;
        }
        // No current prepared copy: converted in idle time
        lock();
        queueConversion(job.source.tile, job.source.region);
        unlock();
    }
    if (rendered) {
        lock();
        if (pendingWriteCount < MAX_PENDING_WRITES) {
            pendingSourceZoom[pendingWriteCount] = job.source.tile.z;
            pendingWrites[pendingWriteCount++] = tile;
        }
        unlock();
    }
    finish(lane, tile, rendered, job.source.tile.z);
    if (rendered) {
        logInfo("[%d] Tile %d/%d/%d from z%d %s: read %u ms, render %u ms (%s %u, geometry %u, fill %u, %u features)", lane.index, tile.z,
            tile.x, tile.y, job.source.tile.z, job.found == Found::Prepared ? ".prt" : ".pbf", loaded - start, stats.totalMs,
            job.found == Found::Prepared ? "inflate" : "scan", stats.scanMs, stats.geometryMs, stats.fillMs, stats.features);
    }
    return true;
}

void TileWorker::loop(Lane& lane) {
    while (!stopRequested) {
        TileKey tile {};
        uint16_t* pixels = nullptr;
        // Tiles on screen first, then the SD card cache, then the tiles around the screen, then conversions
        bool visible = true;
        if (!claimNext(lane, true, tile, pixels)) {
            if (writeOne(lane)) {
                continue;
            }
            visible = false;
            // Around the screen and other idle work only after a while without input
            if (!idle || !claimNext(lane, false, tile, pixels)) {
                if (!idle || (!convertOne(lane) && !tidyCache())) {
                    vTaskDelay(pdMS_TO_TICKS(IDLE_WAIT_MS));
                }
                continue;
            }
        }
        lock();
        lane.prefetching = !visible;
        lane.cancel = false;
        unlock();
        busyLanes++;
        if (!produce(lane, tile, pixels, visible)) {
            // Gave way to more urgent card work: the slot is freed and the tile claimed again when it is still wanted
            lock();
            tileCache.release(tile);
            lane.active = false;
            unlock();
        }
        busyLanes--;
    }
}

} // namespace maps
