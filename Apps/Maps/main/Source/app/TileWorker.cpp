// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileWorker.h"
#include "Downloader.h"

#include "../platform/Platform.h"
#include "../render/TileCodec.h"

#include <esp_heap_caps.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

/** Rendering, and conversions, where ROM tdefl alone needs about 3-4 KB of stack. */
/** tdefl, which deflates prepared tiles, keeps about 6 KB of Huffman tables on the stack: 8 KB left 600 bytes. */
constexpr uint32_t TASK_STACK = 10240;
/** Cache slots kept free of wanted tiles, for the ancestors magnified while a tile renders. */
constexpr int CACHE_SPARE_SLOTS = 6;
/** Lanes keep a file buffer this large between tiles, larger ones (dense z14 or low-zoom tiles) are freed after use. */
constexpr size_t KEEP_FILE_BUFFER = 256 * 1024;
constexpr UBaseType_t TASK_PRIORITY = 4;
constexpr uint32_t IDLE_WAIT_MS = 30;
/** A conversion starts only with a PSRAM block this large free (the compressor needs 320 KB in one block), and this
 * much PSRAM free in all for the original, the features and the labels. */
constexpr size_t CONVERT_MIN_BLOCK = 336 * 1024;
constexpr size_t CONVERT_MIN_FREE = 900 * 1024;
/** While a download runs, a tile on screen is prepared only with this much more PSRAM free, for the download's bodies. */
constexpr size_t CONVERT_DOWNLOAD_SPARE = 400 * 1024;

bool memoryForConversion() {
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= CONVERT_MIN_BLOCK &&
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >= CONVERT_MIN_FREE;
}
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
    /** The files the tile needs (PART_* of PreparedTile.h). */
    uint8_t parts;
    bool yielded;
    /** The original is not there (any more): moved into another region or deleted. */
    bool missing;
};

bool convertReadJob(IoTask& io, void* context) {
    auto* job = static_cast<ConvertReadJob*>(context);
    char path[PATH_MAX_LENGTH];
    char prepared[PATH_MAX_LENGTH];
    if (!tilePath(path, sizeof(path), job->root, job->region, job->tile) ||
        !preparedPath(prepared, sizeof(prepared), job->root, job->region, job->tile)) {
        return false;
    }
    if (!fileSize(path, job->size)) {
        job->missing = true;
        return false;
    }
    // Queued twice, or converted by the other lane meanwhile
    job->parts = PreparedScanner::partsNeeded(prepared, job->tile, job->size);
    job->current = job->parts == 0;
    const bool ok = job->current || io.readFile(path, *job->buffer);
    job->yielded = io.yielded();
    return ok;
}

struct ChunkReadJob {
    const char* path;
    uint32_t offset;
    uint8_t* buffer;
    size_t size;
    size_t count;
};

bool chunkReadJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<ChunkReadJob*>(context);
    FILE* file = fopen(job->path, "rb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = fseek(file, static_cast<long>(job->offset), SEEK_SET) == 0;
    job->count = ok ? fread(job->buffer, 1, job->size, file) : 0;
    const bool failed = !ok || (job->count == 0 && ferror(file));
    fclose(file);
    return !failed;
}

/**
 * Reads a file a chunk at a time through the I/O task, for originals too large for one block of PSRAM (low-zoom tiles
 * of 1 MB and more). Each chunk is its own job, so tiles on screen are read in between.
 */
class CardInput final : public ByteInput {
    IoTask* io;
    const char* path;
    IoPriority priority;
    uint8_t* chunk = nullptr;
    uint32_t chunkOffset = 0;
    size_t chunkLength = 0;
    size_t chunkPosition = 0;
    bool failed = false;

public:
    CardInput(IoTask* io, const char* path, IoPriority priority = IoPriority::Background) : io(io), path(path), priority(priority) {
        chunk = static_cast<uint8_t*>(largeAlloc(IoTask::CHUNK_BYTES));
    }
    ~CardInput() override { largeFree(chunk); }

    bool isReady() const { return chunk != nullptr; }

    int read(uint8_t* buffer, size_t size) override {
        size_t done = 0;
        while (done < size && !failed) {
            if (chunkPosition == chunkLength) {
                ChunkReadJob job { path, chunkOffset + static_cast<uint32_t>(chunkLength), chunk, IoTask::CHUNK_BYTES, 0 };
                if (!io->run(priority, chunkReadJob, &job)) {
                    failed = true;
                    break;
                }
                chunkOffset = job.offset;
                chunkLength = job.count;
                chunkPosition = 0;
                if (chunkLength == 0) {
                    break;
                }
            }
            const size_t n = std::min(size - done, chunkLength - chunkPosition);
            memcpy(buffer + done, chunk + chunkPosition, n);
            chunkPosition += n;
            done += n;
        }
        return failed && done == 0 ? -1 : static_cast<int>(done);
    }

    bool rewind() override {
        chunkOffset = 0;
        chunkLength = 0;
        chunkPosition = 0;
        failed = false;
        return true;
    }
};

struct ConvertWriteJob {
    const char* root;
    const char* region;
    TileKey tile;
    /** nullptr for a file that is current. */
    const ByteWriter* bytes;
    const ByteWriter* labels;
    const ByteWriter* graph;
    bool yielded;
};

/** Writes one prepared file through its temporary name. */
bool writePreparedFile(IoTask& io, const ConvertWriteJob& job, PreparedFile file, const ByteWriter& bytes) {
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH];
    return preparedFilePath(path, sizeof(path), job.root, job.region, job.tile, file) &&
        preparedFileTempPath(temporary, sizeof(temporary), job.root, job.region, job.tile, file) &&
        io.writeFile(path, temporary, bytes.bytes(), bytes.size(), true);
}

/**
 * Records in the .prt header that its companion files are current now: byte 9 is the label file version, byte 7 the
 * graph file version. Two bytes in one sector, written in place, for a companion made again without the geometry.
 */
bool markCompanions(const char* path) {
    FILE* file = fopen(path, "r+b");
    if (file == nullptr) {
        return false;
    }
    const uint8_t graph = PREPARED_GRAPH_FORMAT_VERSION;
    const uint8_t label = PREPARED_LABEL_FORMAT_VERSION;
    const bool ok = fseek(file, 7, SEEK_SET) == 0 && fwrite(&graph, 1, 1, file) == 1 && fseek(file, 9, SEEK_SET) == 0 &&
        fwrite(&label, 1, 1, file) == 1;
    return fclose(file) == 0 && ok;
}

bool convertWriteJob(IoTask& io, void* context) {
    auto* job = static_cast<ConvertWriteJob*>(context);
    // The companions first: a .prt of the new version says they are there
    bool ok = job->labels == nullptr || writePreparedFile(io, *job, PreparedFile::Labels, *job->labels);
    ok = ok && (job->graph == nullptr || writePreparedFile(io, *job, PreparedFile::Graph, *job->graph));
    if (ok && job->bytes != nullptr) {
        ok = writePreparedFile(io, *job, PreparedFile::Geometry, *job->bytes);
    } else if (ok) {
        char path[PATH_MAX_LENGTH];
        ok = preparedFilePath(path, sizeof(path), job->root, job->region, job->tile, PreparedFile::Geometry) && markCompanions(path);
    }
    job->yielded = io.yielded();
    return ok;
}

struct LabelReadJob {
    const char* root;
    const char* region;
    TileKey tile;
    bool numbers;
    FileBuffer* buffer;
    PreparedLabelHeader header;
    bool yielded;
};

/** Reads the header of a .prl file and its labels, with the house numbers only when they are wanted. */
bool labelReadJob(IoTask& io, void* context) {
    auto* job = static_cast<LabelReadJob*>(context);
    char path[PATH_MAX_LENGTH];
    if (!preparedFilePath(path, sizeof(path), job->root, job->region, job->tile, PreparedFile::Labels)) {
        return false;
    }
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return false;
    }
    uint8_t head[PREPARED_LABEL_HEADER_SIZE];
    const bool read = fread(head, 1, sizeof(head), file) == sizeof(head);
    fclose(file);
    if (!read || !readPreparedLabelHeader(head, sizeof(head), job->header) || !(job->header.tile == job->tile) ||
        job->header.version != PREPARED_LABEL_FORMAT_VERSION) {
        return false;
    }
    const size_t wanted = PREPARED_LABEL_HEADER_SIZE + LABEL_INDEX_SIZE + job->header.mainStored + (job->numbers ? job->header.numberStored : 0);
    const bool ok = io.readFile(path, *job->buffer, wanted);
    job->yielded = io.yielded();
    return ok;
}

struct FindJob {
    const TileStore* store;
    TileKey tile;
    TileFile file;
    bool found;
};

bool findJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<FindJob*>(context);
    job->found = job->store->find(job->tile, job->file);
    return true;
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

bool TileWorker::relieveMemory(size_t bytes, size_t totalBytes, int minSlots) {
    lock();
    const int before = tileCache.slotCount();
    // Never fewer than the tiles on screen (9 in a turned view), or they are drawn again and again
    const int keep = std::max(minSlots > 0 ? minSlots : MIN_CACHE_SLOTS, visibleWanted);
    while ((heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < bytes || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < totalBytes) &&
        tileCache.slotCount() > keep && tileCache.freeSlot()) {
    }
    const int after = tileCache.slotCount();
    unlock();
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    logInfo("Memory for a %u KB block: %d of %d cache slots freed, largest block %u KB", static_cast<unsigned>(bytes / 1024), before - after,
        before, static_cast<unsigned>(largest / 1024));
    return largest >= bytes;
}

bool TileWorker::restoreMemory() {
    lock();
    const int missing = tileCache.regrow();
    const int slots = tileCache.slotCount();
    unlock();
    if (missing == 0) {
        logInfo("Tile cache back to %d slots", slots);
    }
    return missing == 0;
}

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
    styleCacheId(*palette, styleId, sizeof(styleId));
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
        if (labelStore != nullptr) {
            lane.renderer.setLabelHook([](const TileKey& source, const uint8_t* section, size_t size, int count, void* context) {
                static_cast<LabelStore*>(context)->put(source, section, size, count);
            }, labelStore);
        }
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
    // Fewer than the cache holds, leaving room for the magnified stand-ins: wanting more made the ring around the
    // screen evict the visible tiles, which were then rendered again, several times over
    const int room = std::max(1, tileCache.slotCount() - CACHE_SPARE_SLOTS);
    wantedCount = std::min({ count, static_cast<int>(MAX_WANTED), room });
    visibleWanted = visibleCount < wantedCount ? visibleCount : wantedCount;
    memcpy(wanted, tiles, sizeof(TileKey) * wantedCount);
    // The visible tiles count as just used, so tiles rendered for the ring evict others first. One that was drawn
    // from coarser data while it was still in the ring is a gap now that it is on screen.
    for (int i = 0; i < visibleWanted; i++) {
        tileCache.find(wanted[i]);
        const int sourceZoom = tileCache.sourceZoomOf(wanted[i]);
        const int bestZoom = wanted[i].z < MAX_SOURCE_ZOOM ? wanted[i].z : MAX_SOURCE_ZOOM;
        if (sourceZoom >= 0 && sourceZoom < bestZoom) {
            addGap(ancestorOf(wanted[i], bestZoom));
        }
    }
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

void TileWorker::tilesArrived(const TileKey* tiles, int count) {
    lock();
    missingCount = 0;
    missingNext = 0;
    if (count >= 0) {
        tileCache.dropMagnified(tiles, count);
    } else if (conversionsPaused) {
        magnifiedOutdated = true;
    } else {
        tileCache.dropMagnified();
    }
    unlock();
    // New originals get their prepared copies in idle time
    rescanRequested = true;
    changed = true;
}

void TileWorker::setConversionsPaused(bool paused) {
    if (paused == conversionsPaused) {
        return;
    }
    lock();
    conversionsPaused = paused;
    if (!paused) {
        // The tiles of the download are prepared once the map was left alone for a while from now on
        idleSince = xTaskGetTickCount();
    }
    if (!paused && magnifiedOutdated) {
        magnifiedOutdated = false;
        tileCache.dropMagnified();
        changed = true;
    }
    unlock();
}

bool TileWorker::queueConversion(const TileKey& tile, int region, bool urgent, bool split) {
    for (int i = 0; i < failedConversionCount; i++) {
        if (failedConversions[i] == tile) {
            return true;
        }
    }
    for (int i = 0; i < conversionCount; i++) {
        if (conversions[i].tile == tile) {
            conversions[i].split = conversions[i].split || split;
            if (urgent && !conversions[i].urgent) {
                // Now on screen: first in line (during a download only the first, urgent one is prepared)
                Conversion entry = conversions[i];
                entry.urgent = true;
                for (int j = i; j > 0; j--) {
                    conversions[j] = conversions[j - 1];
                }
                conversions[0] = entry;
            }
            return true;
        }
    }
    for (int i = 0; i < laneCount; i++) {
        if (lanes[i].converting && lanes[i].convertingTile == tile) {
            return true;
        }
    }
    if (urgent) {
        const int kept = conversionCount < MAX_CONVERSIONS ? conversionCount : MAX_CONVERSIONS - 1;
        for (int i = kept; i > 0; i--) {
            conversions[i] = conversions[i - 1];
        }
        conversions[0] = { tile, region, true, split };
        conversionCount = kept + 1;
        return true;
    }
    if (conversionCount >= MAX_CONVERSIONS) {
        return false;
    }
    conversions[conversionCount++] = { tile, region, false, split };
    return true;
}

bool TileWorker::collectFromScan(const TileKey& tile, int region, bool split, void* context) {
    auto* found = static_cast<ScanResult*>(context);
    if (found->count >= found->room) {
        return false;
    }
    found->items[found->count++] = { tile, region, false, split };
    return true;
}

bool TileWorker::loadLabels(Lane& lane) {
    LabelStore::Request request {};
    if (labelStore == nullptr || !labelStore->takeRequest(request)) {
        return false;
    }
    char region[REGION_NAME_MAX + 1] = "";
    lock();
    if (request.region >= 0 && request.region < store->regionCount()) {
        snprintf(region, sizeof(region), "%s", store->regionName(request.region));
    }
    unlock();
    if (region[0] == '\0') {
        labelStore->done(request.tile);
        return true;
    }
    // However it ends, the request is no longer being read
    struct Done {
        LabelStore* store;
        TileKey tile;
        ~Done() { store->done(tile); }
    } done { labelStore, request.tile };
    const uint32_t start = nowMs();
    // The lane's file buffer: the file it held for its last tile is read again when needed
    lane.heldKind = static_cast<uint8_t>(Found::Nothing);
    LabelReadJob read { root, region, request.tile, request.numbers, &lane.fileBuffer, {}, false };
    const bool ok = io->run(IoPriority::Visible, labelReadJob, &read);
    if (!ok) {
        if (read.yielded) {
            labelStore->done(request.tile);
            labelStore->request(request.tile, request.region);
        }
        return true;
    }
    const uint32_t loaded = nowMs();
    const PreparedLabelHeader& header = read.header;
    const size_t size = header.mainSize + (request.numbers ? header.numberSize : 0);
    const int count = static_cast<int>(header.mainCount + (request.numbers ? header.numberCount : 0));
    // The cells first, then the records; a tile without labels keeps an empty entry, so it is not asked for again
    auto* block = static_cast<uint8_t*>(largeAlloc(sizeof(LabelCells) + size));
    bool inflated = block != nullptr &&
        readLabelCells(lane.fileBuffer.bytes() + PREPARED_LABEL_HEADER_SIZE, header.mainSize, header.numberSize, request.numbers,
            *reinterpret_cast<LabelCells*>(block));
    uint8_t* bytes = block != nullptr ? block + sizeof(LabelCells) : nullptr;
    const uint8_t* stored = lane.fileBuffer.bytes() + PREPARED_LABEL_HEADER_SIZE + LABEL_INDEX_SIZE;
    const bool deflated = (header.flags & PREPARED_FLAG_DEFLATED) != 0;
    // Part A, then part B right after it, so the records read as one section
    const uint32_t partSize[2] = { header.mainSize, request.numbers ? header.numberSize : 0 };
    const uint32_t partStored[2] = { header.mainStored, request.numbers ? header.numberStored : 0 };
    size_t offset = 0;
    for (int part = 0; part < 2 && inflated; part++) {
        if (partSize[part] == 0) {
            stored += partStored[part];
            continue;
        }
        if (deflated) {
            MemoryInput input(stored, partStored[part]);
            inflated = lane.renderer.tileDecoder().inflate(&input, bytes + offset, partSize[part]);
        } else {
            inflated = partStored[part] == partSize[part];
            if (inflated) {
                memcpy(bytes + offset, stored, partSize[part]);
            }
        }
        stored += partStored[part];
        offset += partSize[part];
    }
    lane.fileBuffer.trim(KEEP_FILE_BUFFER);
    if (!inflated) {
        largeFree(block);
        logError("[%d] Labels of %d/%d/%d do not read", lane.index, request.tile.z, request.tile.x, request.tile.y);
        return true;
    }
    labelStore->adopt(request.tile, block, size, count, request.numbers, request.region);
    logInfo("[%d] Labels %d/%d/%d%s: %u bytes, read %u ms, inflate %u ms", lane.index, request.tile.z, request.tile.x, request.tile.y,
        request.numbers ? " with house numbers" : "", static_cast<unsigned>(size), static_cast<unsigned>(loaded - start),
        static_cast<unsigned>(nowMs() - loaded));
    return true;
}

bool TileWorker::convertOne(Lane& lane) {
    if (routing) {
        return false;
    }
    // While a download runs (it leaves PSRAM fragmented), only the tiles on screen are prepared, so their POIs and house
    // numbers show at once instead of after the download; the others wait until it is over. The download keeps memory
    // to spare: the tile cache gives up slots that are not on screen for it (they come back after the download).
    if (conversionsPaused) {
        lock();
        const bool onScreen = conversionCount > 0 && conversions[0].urgent;
        unlock();
        if (!onScreen) {
            return false;
        }
        const size_t wanted = CONVERT_MIN_FREE + CONVERT_DOWNLOAD_SPARE;
        if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < wanted || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < CONVERT_MIN_BLOCK) {
            const TickType_t now = xTaskGetTickCount();
            if (static_cast<int32_t>(now - conversionsWaitUntil.load()) < 0) {
                return false;
            }
            relieveMemory(CONVERT_MIN_BLOCK, wanted);
            if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < wanted || heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < CONVERT_MIN_BLOCK) {
                conversionsWaitUntil = now + pdMS_TO_TICKS(DOWNLOAD_BACKOFF_MS);
                return false;
            }
        }
    }
    if (lane.trimBuffers.exchange(false)) {
        lane.heldKind = static_cast<uint8_t>(Found::Nothing);
        lane.fileBuffer.trim(0);
        lane.converter.trim(0);
        lane.converted.release();
    }
    const TickType_t now = xTaskGetTickCount();
    if (static_cast<int32_t>(now - conversionsWaitUntil.load()) < 0) {
        return false;
    }
    // Tiles not on screen wait until the map was left alone for a while: they are queued now, prepared later
    const bool settled = now - idleSince.load() >= pdMS_TO_TICKS(BACKGROUND_PREPARE_MS);
    lock();
    Conversion job {};
    bool found = false;
    bool otherConverting = false;
    for (int i = 0; i < laneCount; i++) {
        otherConverting = otherConverting || lanes[i].converting;
    }
    if (conversionCount > 0 && !otherConverting && (conversions[0].urgent || settled)) {
        if (!memoryForConversion()) {
            // The tile cache gives up its least recently used slots for it (they come back when nothing is left to
            // prepare). Without enough memory even then the tile stays queued, and the next try is after a pause
            const int queued = conversionCount;
            const bool firstTry = !lane.triedTrim;
            lane.triedTrim = true;
            for (int i = 0; i < laneCount && firstTry; i++) {
                lanes[i].trimBuffers = true;
            }
            unlock();
            if (firstTry) {
                // The lanes free their kept buffers first, then this is tried again
                return true;
            }
            if (relieveMemory(CONVERT_MIN_BLOCK) && memoryForConversion()) {
                return true;
            }
            conversionsWaitUntil = now + pdMS_TO_TICKS(MEMORY_BACKOFF_MS);
            lane.triedTrim = false;
            logInfo("Preparing tiles waits for memory (largest free PSRAM block %u KB, %u KB free, %d queued)",
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024),
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024), queued);
            return false;
        }
        lane.triedTrim = false;
        // One conversion at a time: each needs about 0.4 MB of PSRAM, which a download needs as well.
        // The tiles on screen first, then the oldest, so the tiles just drawn from originals come before the scanned ones
        job = conversions[0];
        for (int i = 1; i < conversionCount; i++) {
            conversions[i - 1] = conversions[i];
        }
        conversionCount--;
        found = true;
        lane.converting = true;
        lane.convertingTile = job.tile;
    } else if (settled && (rescanRequested || !scanner.isComplete()) && !scanning) {
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
            queueConversion(result.items[i].tile, result.items[i].region, false, result.items[i].split);
        }
        scanning = false;
        unlock();
        return scan.more || result.count > 0;
    }

    const uint32_t start = nowMs();
    // The conversion reuses the lane's buffer
    lane.heldKind = static_cast<uint8_t>(Found::Nothing);
    ConvertReadJob read { root, region, job.tile, &lane.fileBuffer, 0, false, 0, false, region[0] == '\0' };
    bool ok = region[0] != '\0' && io->run(IoPriority::Background, convertReadJob, &read);
    if (read.missing) {
        // A download moved it (browsed tiles go into the new region). A tile on screen is queued again where it is
        // now, still first; the others the scan finds again
        FindJob findMoved { store, job.tile, {}, false };
        const bool found = job.urgent && io->run(IoPriority::Background, findJob, &findMoved) && findMoved.found &&
            findMoved.file.tile == job.tile && findMoved.file.region != job.region;
        const TileFile& moved = findMoved.file;
        logInfo("[%d] %d/%d/%d is no longer in %s, %s", lane.index, job.tile.z, job.tile.x, job.tile.y, region[0] != '\0' ? region : "its region",
            found ? "queued again in its new region" : "not prepared there");
        lane.fileBuffer.trim(KEEP_FILE_BUFFER);
        lock();
        lane.converting = false;
        if (found) {
            queueConversion(job.tile, moved.region, true, job.split);
        }
        unlock();
        return true;
    }
    const uint32_t loaded = nowMs();
    bool requeue = read.yielded;
    const char* reason = "reading the card";
    // No block of PSRAM was large enough for the whole original: it is read a chunk at a time instead
    const bool streamed = !ok && !requeue && read.size > 0;
    if (streamed) {
        lane.fileBuffer.trim(0);
    }
    if ((ok && !read.current) || streamed) {
        ConvertStats stats {};
        const ByteWriter* writeGeometry = nullptr;
        const ByteWriter* writeLabels = nullptr;
        const ByteWriter* writeGraph = nullptr;
        GraphStats graphStats {};
        char path[PATH_MAX_LENGTH];
        CardInput cardInput(io, path);
        MemoryInput memoryInput(lane.fileBuffer.bytes(), lane.fileBuffer.size());
        ByteInput* input = &memoryInput;
        if (streamed) {
            ok = tilePath(path, sizeof(path), root, region, job.tile) && cardInput.isReady();
            input = &cardInput;
        }
        ByteWriter* geometry = (read.parts & PART_GEOMETRY) != 0 ? &lane.converted : nullptr;
        ByteWriter* labels = (read.parts & PART_LABELS) != 0 ? &lane.convertedLabels : nullptr;
        if (ok && (geometry != nullptr || labels != nullptr)) {
            ok = lane.converter.convert(lane.renderer.tileDecoder(), input, job.tile, read.size, version, deflatePrepared, geometry, labels, &stats);
            reason = ok ? "writing the card" : lane.converter.failure();
            writeGeometry = geometry;
            writeLabels = labels;
        }
        if (ok && (read.parts & PART_GRAPH) != 0) {
            // The routing graph of a z14 tile, from the original read again from the start
            CardInput graphCard(io, path);
            MemoryInput graphMemory(lane.fileBuffer.bytes(), lane.fileBuffer.size());
            ByteInput* graphInput = streamed ? static_cast<ByteInput*>(&graphCard) : &graphMemory;
            ok = lane.graphBuilder.build(lane.renderer.tileDecoder(), graphInput, job.tile, read.size, version, true, lane.convertedGraph, &graphStats);
            reason = ok ? "writing the card" : lane.graphBuilder.failure();
            writeGraph = &lane.convertedGraph;
        }
        const uint32_t convertedAt = nowMs();
        ConvertWriteJob write { root, region, job.tile, writeGeometry, writeLabels, writeGraph, false };
        ok = ok && io->run(IoPriority::Background, convertWriteJob, &write);
        requeue = write.yielded;
        if (ok) {
            logInfo("[%d] %s %d/%d/%d%s: %u -> %u + %u + %u bytes (.prt + .prl + .prg), read %u ms, convert %u ms (graph %u ms, %u nodes, "
                    "%u edges), write %u ms",
                lane.index, job.split ? "Split" : "Prepared", job.tile.z, job.tile.x, job.tile.y, streamed ? " in chunks" : "",
                static_cast<unsigned>(read.size), writeGeometry != nullptr ? static_cast<unsigned>(writeGeometry->size()) : 0,
                writeLabels != nullptr ? static_cast<unsigned>(writeLabels->size()) : 0,
                writeGraph != nullptr ? static_cast<unsigned>(writeGraph->size()) : 0, loaded - start, convertedAt - loaded,
                static_cast<unsigned>(graphStats.totalMs), static_cast<unsigned>(graphStats.nodes), static_cast<unsigned>(graphStats.edges),
                nowMs() - convertedAt);
            // Tiles drawn from the original or an older prepared copy are drawn again from the new one, which has labels
            preparedCount++;
            if (writeGeometry != nullptr || writeLabels != nullptr) {
                preparedGeneration++;
                lock();
                tileCache.dropMagnified(&job.tile, 1);
                changed = true;
                unlock();
            }
        }
    }
    // A read or write that gave way to tiles on screen is tried again later
    lane.fileBuffer.trim(KEEP_FILE_BUFFER);
    lane.converter.trim(KEEP_FILE_BUFFER / 4);
    if (lane.convertedLabels.allocated() > KEEP_FILE_BUFFER / 4) {
        lane.convertedLabels.release();
    }
    // The graph builder's working memory goes back after each tile: graphs are made once per tile
    lane.graphBuilder.trim();
    lane.convertedGraph.release();
    bool retry = false;
    if (!ok && !requeue && !memoryForConversion()) {
        // Out of memory, not a bad tile: it stays queued and conversions pause, it does not count as a failure
        conversionsWaitUntil = xTaskGetTickCount() + pdMS_TO_TICKS(MEMORY_BACKOFF_MS);
        logInfo("[%d] Preparing %d/%d/%d waits for memory (%s, largest free PSRAM block %u KB)", lane.index, job.tile.z, job.tile.x,
            job.tile.y, reason, static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        requeue = true;
    } else if (!ok && !requeue) {
        const auto largest = static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        lock();
        // A tile that failed once, usually for want of memory, is tried once more at the end of the queue
        retry = std::find(retriedConversions, retriedConversions + retriedConversionCount, job.tile) ==
            retriedConversions + retriedConversionCount;
        if (retry) {
            retriedConversions[retriedConversionNext] = job.tile;
            retriedConversionNext = (retriedConversionNext + 1) % MAX_FAILED_CONVERSIONS;
            retriedConversionCount = std::min(retriedConversionCount + 1, MAX_FAILED_CONVERSIONS);
        }
        unlock();
        logError("[%d] Could not prepare %d/%d/%d (%s, largest free PSRAM block %u bytes), %s", lane.index, job.tile.z, job.tile.x,
            job.tile.y, reason, largest, retry ? "tried again later" : "not tried again until the map reopens");
        requeue = retry;
    }
    lock();
    if (!ok && !requeue) {
        failedConversions[failedConversionNext] = job.tile;
        failedConversionNext = (failedConversionNext + 1) % MAX_FAILED_CONVERSIONS;
        failedConversionCount = std::min(failedConversionCount + 1, MAX_FAILED_CONVERSIONS);
    }
    lane.converting = false;
    if (requeue) {
        queueConversion(job.tile, job.region, job.urgent, job.split);
    }
    unlock();
    // Something was done, or the card is busy with more urgent work
    return true;
}

void TileWorker::preparation(int& done, int& waiting, int& splitting) {
    lock();
    waiting = conversionCount;
    splitting = 0;
    for (int i = 0; i < conversionCount; i++) {
        splitting += conversions[i].split ? 1 : 0;
    }
    for (int i = 0; i < laneCount; i++) {
        waiting += lanes[i].converting ? 1 : 0;
    }
    unlock();
    done = preparedCount.load();
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
    if (missingRetryAt != 0 && static_cast<int32_t>(xTaskGetTickCount() - missingRetryAt) >= 0) {
        // Tiles that failed for want of memory get another try
        missingRetryAt = 0;
        missingCount = 0;
        missingNext = 0;
    }
    bool found = false;
    const int first = visible ? 0 : visibleWanted;
    // The ring around the screen only fits in what the cache holds now: while a route search has borrowed slots, it
    // evicted the visible tiles, which were drawn again (the map blinked)
    const int last = visible ? visibleWanted : std::min(wantedCount, std::max(visibleWanted, tileCache.slotCount() - CACHE_SPARE_SLOTS));
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
        // Drawn while it was in the ring, then the view settled with it on screen: want() looked before it was
        // finished, so the gap is noted here, or it stayed drawn from a coarser zoom level and was never fetched
        const int bestZoom = tile.z < MAX_SOURCE_ZOOM ? tile.z : MAX_SOURCE_ZOOM;
        for (int i = 0; i < visibleWanted && sourceZoom < bestZoom; i++) {
            if (wanted[i] == tile) {
                addGap(ancestorOf(tile, bestZoom));
                break;
            }
        }
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
    Lane& lane = *job->lane;
    FileBuffer& buffer = lane.fileBuffer;
    job->found = Found::Nothing;
    job->yielded = false;
    const bool hasSource = worker->store->find(job->tile, job->source);
    char path[PATH_MAX_LENGTH];
    // Only tiles the index knows are read, a missing file costs a folder lookup on the card
    worker->lock();
    const bool indexed = job->tryCache && worker->sdCache && (worker->cacheIndex.contains(job->tile) || !worker->cacheIndex.isComplete());
    worker->unlock();
    const auto held = static_cast<Found>(lane.heldKind);
    if (indexed) {
        lane.heldKind = static_cast<uint8_t>(Found::Nothing);
    }
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
    // The file the lane read for the last tile, when it is this one's source too
    if (!indexed && buffer.size() > 0 && lane.heldTile == job->source.tile && lane.heldRegion == job->source.region &&
        ((held == Found::Prepared && !job->skipPrepared) ||
            (held == Found::Original && lane.heldGeneration == worker->preparedGeneration.load()))) {
        job->found = held;
        job->olderPrepared = lane.heldOlder;
        job->splitPrepared = lane.heldSplit;
        return true;
    }
    lane.heldKind = static_cast<uint8_t>(Found::Nothing);
    lane.heldTile = job->source.tile;
    lane.heldRegion = job->source.region;
    lane.heldGeneration = worker->preparedGeneration.load();
    PreparedHeader header {};
    if (!job->skipPrepared && preparedPath(path, sizeof(path), worker->root, worker->store->regionName(job->source.region), job->source.tile) &&
        io.readFile(path, buffer) && readPreparedHeader(buffer.bytes(), buffer.size(), header) &&
        isPreparedReadable(header, job->source.tile, job->source.size)) {
        job->found = Found::Prepared;
        job->olderPrepared = preparedPartsNeeded(&header, job->source.tile, job->source.size) != 0;
        job->splitPrepared = header.version >= PREPARED_SPLIT_VERSION;
        lane.heldKind = static_cast<uint8_t>(Found::Prepared);
        lane.heldOlder = job->olderPrepared;
        lane.heldSplit = job->splitPrepared;
        return true;
    }
    if (!io.yielded() && io.readFile(job->source.path, buffer)) {
        job->found = Found::Original;
        lane.heldKind = static_cast<uint8_t>(Found::Original);
        lane.heldOlder = false;
        return true;
    }
    job->yielded = io.yielded();
    job->noMemoryBytes = buffer.lastShortfall();
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
    if (job.found == Found::Nothing && job.noMemoryBytes > 0 && visible) {
        // A large low-zoom tile on screen and no PSRAM block for its file: preparing gives way, the least recently
        // used cache slots make room, and it is read again. Without this it stayed drawn from a coarser zoom level
        logInfo("[%d] Tile %d/%d/%d: no %u KB block of PSRAM for its data, making room", lane.index, tile.z, tile.x, tile.y,
            static_cast<unsigned>(job.noMemoryBytes / 1024));
        conversionsWaitUntil = xTaskGetTickCount() + pdMS_TO_TICKS(MEMORY_BACKOFF_MS);
        for (int i = 0; i < laneCount; i++) {
            lanes[i].trimBuffers = true;
        }
        lane.fileBuffer.trim(0);
        relieveMemory(job.noMemoryBytes + 0x10000);
        job.noMemoryBytes = 0;
        io->run(priority, loadJob, &job);
        if (job.yielded) {
            return false;
        }
    }
    // Still no block that large: the original is drawn reading it from the card a chunk at a time, as preparing does
    bool streamedOk = false;
    RenderStats streamStats {};
    if (job.found == Found::Nothing && job.noMemoryBytes > 0 && job.source.path[0] != '\0') {
        CardInput input(io, job.source.path, priority);
        streamedOk = input.isReady() && lane.renderer.render(&input, job.source.tile, tile, *palette, pixels, &streamStats);
        if (lane.renderer.wasCancelled()) {
            return false;
        }
    }
    if (job.found == Found::Nothing && job.noMemoryBytes > 0 && !streamedOk) {
        // Not even that: tried again in a few seconds, not left drawn from a coarser zoom level
        logError("[%d] Tile %d/%d/%d: no %u KB block of PSRAM for its data (largest %u KB), tried again in %u s", lane.index, tile.z,
            tile.x, tile.y, static_cast<unsigned>(job.noMemoryBytes / 1024),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024), static_cast<unsigned>(MEMORY_RETRY_MS / 1000));
        lock();
        if (missingRetryAt == 0) {
            missingRetryAt = xTaskGetTickCount() + pdMS_TO_TICKS(MEMORY_RETRY_MS);
        }
        unlock();
    }
    const uint32_t loaded = nowMs();
    // A tile on screen without data of its own zoom level is a gap that browsing can fill
    if (visible) {
        const int bestZoom = tile.z < MAX_SOURCE_ZOOM ? tile.z : MAX_SOURCE_ZOOM;
        int haveZoom = -1;
        if (job.found == Found::Cached) {
            haveZoom = job.cachedSourceZoom;
        } else if (job.found != Found::Nothing || streamedOk) {
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
    if (streamedOk) {
        rendered = true;
        stats = streamStats;
        logInfo("[%d] Tile %d/%d/%d drawn from its original read in chunks (no %u KB block of PSRAM)", lane.index, tile.z, tile.x,
            tile.y, static_cast<unsigned>(job.noMemoryBytes / 1024));
        // Its prepared copy is small and makes the next drawing cheap
        lock();
        queueConversion(job.source.tile, job.source.region, true);
        unlock();
    }
    if (job.found == Found::Prepared) {
        rendered = lane.renderer.renderPrepared(lane.fileBuffer.bytes(), lane.fileBuffer.size(), tile, *palette, pixels, &stats);
        if (lane.renderer.wasCancelled()) {
            return false;
        }
        if (rendered && job.olderPrepared) {
            // A tile on screen from an older format is made again (split) before the idle scan gets to it
            lock();
            queueConversion(job.source.tile, job.source.region, true, !job.splitPrepared);
            unlock();
        }
        if (rendered && job.splitPrepared && labelStore != nullptr) {
            // Its labels are in the .prl file: a lane reads them once no tile on screen waits
            labelStore->request(job.source.tile, job.source.region);
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
        // No current prepared copy: converted in idle time, before the tiles the scan found
        lock();
        queueConversion(job.source.tile, job.source.region, true);
        unlock();
    }
    lane.fileBuffer.trim(KEEP_FILE_BUFFER);
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
        // A TLS handshake is 2-3.6 s of CPU at a lower priority: rendering beside it on the same core starved it and
        // the core's idle task, which the task watchdog reported. It gets the core, at most 5 s.
        for (int wait = 0; lane.index == Downloader::NETWORK_CORE && Downloader::isHandshaking() && wait < 250 && !stopRequested; wait++) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        TileKey tile {};
        uint16_t* pixels = nullptr;
        // Tiles on screen first, then the SD card cache, then the tiles around the screen, then conversions
        bool visible = true;
        if (!claimNext(lane, true, tile, pixels)) {
            if (loadLabels(lane) || writeOne(lane)) {
                continue;
            }
            visible = false;
            // Around the screen and other idle work only after a while without input
            // Not during a route search: the user waits for it, and the cache has lent it most of its slots
            if (!idle || routing || !claimNext(lane, false, tile, pixels)) {
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
        // A run of low-zoom tiles of about 1 s each kept the core's idle task from running for the 5 s the task watchdog allows
        vTaskDelay(1);
    }
}

} // namespace maps
