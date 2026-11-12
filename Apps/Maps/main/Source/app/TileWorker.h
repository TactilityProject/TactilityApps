// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"
#include "LabelStore.h"

#include "../mvt/ByteInput.h"
#include "../render/CacheIndex.h"
#include "../render/CacheWalker.h"
#include "../render/PreparedScanner.h"
#include "../render/PreparedTile.h"
#include "../route/RouteGraph.h"
#include "../render/Style.h"
#include "../render/TileCache.h"
#include "../render/TileRenderer.h"
#include "../tiles/TileStore.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace maps {

/**
 * Fills the tile cache with one render task per core. The lanes never touch the SD card: they hand jobs to the
 * I/O task, which reads whole files into the lane's buffer, so one lane renders while the card serves the other.
 * The view hands over the tiles it wants, nearest first. A tile comes from the rendered tile cache on the SD
 * card when it is there and that cache is switched on, otherwise it is rendered from its prepared copy (.prt),
 * or from the original vector tile while there is no current prepared copy, and written to the SD card cache.
 * The SD card cache is kept per style and palette under cache/<style-id>, below a size cap, deleting the least
 * recently used tiles first. It never touches maps/.
 * Tiles around the screen are only rendered after a second without input (idle), and any input cancels them,
 * so they never delay a tile on screen. In idle time the lanes also convert originals into prepared tiles: the ones
 * just drawn on screen from an original at once, every other tile on the card (found by a PreparedScanner, such as
 * the tiles of a new download) only once the map was left alone for BACKGROUND_PREPARE_MS. When PSRAM has no block
 * large enough, conversions wait MEMORY_BACKOFF_MS and the tile stays queued instead of counting as failed.
 */
class TileWorker {
public:
    static constexpr int MAX_WANTED = 32;
    static constexpr int LANES = 2;

    bool start(TileStore* store, IoTask* io, const Palette* palette, int cacheSlots, const char* cacheRoot);
    /** Where the lanes put the label sections of the prepared tiles they draw. Set before start(). */
    void setLabelStore(LabelStore* store) { labelStore = store; }
    void stop();

    /** Replaces the wanted tiles. The first visibleCount are on screen, the rest are fetched ahead. */
    void want(const TileKey* tiles, int count, int visibleCount);

    /** The cache is shared with the render tasks, so every access happens between lock() and unlock(). */
    void lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(mutex); }
    TileCache& cache() { return tileCache; }
    /**
     * Frees tile cache slots, least recently used first, until PSRAM has a free block of bytes, keeping at least
     * MIN_CACHE_SLOTS. For downloads of large low-zoom tiles late in a session. Returns whether the block is there.
     */
    /** Frees cache slots until PSRAM has a block of bytes and, when given, totalBytes free in all. */
    bool relieveMemory(size_t bytes, size_t totalBytes = 0, int minSlots = 0);
    /** Takes the freed slots back when memory allows; true when none is missing any more. */
    bool restoreMemory();
    /** Whether tiles are queued for preparation or being prepared; the cache stays small for them meanwhile. */
    bool isPreparing() {
        lock();
        bool preparing = conversionCount > 0;
        for (int i = 0; i < laneCount; i++) {
            preparing = preparing || lanes[i].converting;
        }
        unlock();
        return preparing;
    }
    bool cacheShrunk() {
        lock();
        const bool shrunk = tileCache.freedSlots() > 0;
        unlock();
        return shrunk;
    }
    /** relieveMemory() leaves the cache at least this many slots. */
    static constexpr int MIN_CACHE_SLOTS = 12;

    /** True once since the last call when a tile was added to the cache. */
    bool takeChanged() { return changed.exchange(false); }
    bool isBusy() const { return busyLanes.load() > 0; }
    /**
     * Background preparation (new originals, or older prepared tiles made again): tiles done since start, tiles
     * waiting, and of those the older tiles with labels inside that are split into their new files.
     */
    void preparation(int& done, int& waiting, int& splitting);
    TaskHandle_t laneTask(int lane) const { return lane < laneCount ? lanes[lane].task : nullptr; }
    /** The fewest bytes of stack a render lane had left so far. */
    uint32_t stackLeft(int lane) const {
        return lane < laneCount && lanes[lane].task != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(lanes[lane].task)) : 0;
    }
    /** Whether a tile is cached or known to have no data. Call between lock() and unlock(). */
    bool isSettled(const TileKey& tile) const;
    /**
     * New vector tiles are on the card: tiles without data and tiles drawn from lower zoom levels are drawn again,
     * only those the given tiles cover, or all of them when count is -1.
     */
    void tilesArrived(const TileKey* tiles, int count);
    /** Whether new prepared tiles are written deflated. */
    void setDeflatePrepared(bool deflate) { deflatePrepared = deflate; }
    /**
     * No tiles are prepared while paused (during a download), they are prepared afterwards. Tiles on screen are drawn
     * again from the downloaded tiles once, when the pause ends, not after every batch.
     */
    void setConversionsPaused(bool paused);
    /** While a route search runs no tiles are prepared and none around the screen drawn: it needs the card, the memory and the cores. */
    void setRouting(bool value) { routing = value; }
    /** Whether rendered tiles are read from and written to the SD card cache. */
    void setSdCache(bool enabled) { sdCache = enabled; }
    /** Idle: no input for a while, so prefetch, conversions and cache tidying may run. */
    void setIdle(bool value) {
        if (value && !idle) {
            idleSince = xTaskGetTickCount();
        }
        idle = value;
    }
    /** Tiles not on screen are prepared only after this long without input and without a download. */
    static constexpr uint32_t BACKGROUND_PREPARE_MS = 10000;
    /** How long conversions wait after PSRAM had no block large enough for one. */
    static constexpr uint32_t MEMORY_BACKOFF_MS = 30000;
    /** During a download: the next try to make room for a tile on screen. */
    static constexpr uint32_t DOWNLOAD_BACKOFF_MS = 3000;
    /** Input arrived: prefetch reads and renders stop at once, and idle work waits. */
    void cancelPrefetch();
    /**
     * Tiles on screen that were drawn from a lower zoom level or not at all, as the tile of the zoom level that
     * would draw them in full detail (at most z14). Returns how many were written to out, each one once.
     */
    int takeGaps(TileKey* out, int max);

private:
    struct Lane {
        TileWorker* worker;
        int index;
        TileRenderer renderer;
        FileBuffer fileBuffer;
        /**
         * The source file fileBuffer still holds from the last tile, so the tiles drawn magnified from one large
         * low-zoom file read it from the card once instead of once each. Only the lane's own jobs use it, one at a time.
         */
        uint8_t heldKind;
        TileKey heldTile;
        int heldRegion;
        bool heldOlder;
        bool heldSplit;
        uint32_t heldGeneration;
        TaskHandle_t task;
        TileKey current;
        bool active;
        uint16_t* writeBuffer;
        uint16_t* encodeBuffer;
        TileConverter converter;
        ByteWriter converted;
        ByteWriter convertedLabels;
        GraphBuilder graphBuilder;
        ByteWriter convertedGraph;
        /** Inflated labels of a .prl file, handed to the label store. */
        FileBuffer labelFile;
        bool converting;
        TileKey convertingTile;
        bool prefetching;
        std::atomic<bool> cancel;
        /** Preparing waits for memory: the lane frees the buffers it keeps between tiles, so PSRAM has larger blocks. */
        std::atomic<bool> trimBuffers;
        /** Whether the buffers were freed for the conversion now waiting; reset once a conversion starts. */
        bool triedTrim;
    };

    struct Conversion {
        TileKey tile;
        int region;
        /** Drawn on screen from its original: prepared without waiting for BACKGROUND_PREPARE_MS. */
        bool urgent;
        /** An older prepared tile with labels inside, split into its new files. */
        bool split;
    };

    static constexpr int MAX_CONVERSIONS = 32;

    struct ScanResult {
        Conversion items[MAX_CONVERSIONS];
        int count;
        int room;
    };

    /** What the I/O task found for a tile. */
    enum class Found : uint8_t {
        Nothing,
        Cached,
        Prepared,
        Original
    };

    /** Counts finished conversions: an original held in a lane's buffer is not reused once a prepared copy exists. */
    std::atomic<uint32_t> preparedGeneration = 0;
    std::atomic<int> preparedCount = 0;
    std::atomic<int> splitWaiting = 0;

    struct LoadJob {
        TileWorker* worker;
        Lane* lane;
        TileKey tile;
        bool tryCache;
        bool skipPrepared;
        /** The prepared copy is of an older format: drawn as it is, made again soon. */
        bool olderPrepared;
        /** The prepared copy has no labels inside (version 5 on): they are read from its .prl file. */
        bool splitPrepared;
        Found found;
        TileFile source;
        int cachedSourceZoom;
        bool yielded;
        /** The tile's data is on the card but PSRAM had no block this large for it, or 0. */
        size_t noMemoryBytes;
    };

    static void taskMain(void* context);
    void loop(Lane& lane);
    /** Claims the next wanted tile that is on screen, or around the screen when visible is false. */
    bool claimNext(Lane& lane, bool visible, TileKey& tile, uint16_t*& pixels);
    /** Reads what a tile is drawn from: its SD cache file, its prepared copy or its original. */
    static bool loadJob(IoTask& io, void* context);
    /** Produces a claimed tile. Returns false when it yielded to more urgent card work and should be tried again. */
    bool produce(Lane& lane, const TileKey& tile, uint16_t* pixels, bool visible);
    void finish(Lane& lane, const TileKey& tile, bool success, int sourceZoom);
    /** Writes one rendered tile to the SD card cache, only while no tile is waiting to be drawn. */
    bool writeOne(Lane& lane);
    /** Call between lock() and unlock(). */
    void addGap(const TileKey& tile);
    /** Adds a tile to the conversion queue unless it is there already. Call between lock() and unlock(). */
    /** Queues a tile for conversion; urgent ones (on screen) go first and push the last queued one out when full. */
    bool queueConversion(const TileKey& tile, int region, bool urgent = false, bool split = false);
    static bool collectFromScan(const TileKey& tile, int region, bool split, void* context);
    /** Reads the labels of one split tile the label store asked for. Returns false when none were wanted. */
    bool loadLabels(Lane& lane);
    /** Converts one queued tile, or looks at one more folder for tiles to convert. Returns false when there is nothing to do. */
    bool convertOne(Lane& lane);
    /** Deletes the least recently used SD cache tile while the cache is over its cap, or tidies one cache folder. */
    bool tidyCache();
    void saveCacheIndex();
    static void lockIndex(void* context);
    static void unlockIndex(void* context);

    TileStore* store = nullptr;
    LabelStore* labelStore = nullptr;
    IoTask* io = nullptr;
    const Palette* palette = nullptr;
    Lane lanes[LANES] {};
    int laneCount = 0;
    TileCache tileCache;
    char root[PATH_MAX_LENGTH] = "";
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t stopped = nullptr;
    std::atomic<bool> stopRequested = false;
    std::atomic<bool> changed = false;
    std::atomic<int> busyLanes = 0;
    TileKey wanted[MAX_WANTED] {};
    int wantedCount = 0;
    int visibleWanted = 0;
    TileKey missing[64] {};
    int missingCount = 0;
    int missingNext = 0;
    /** Ticks when tiles that failed for want of memory are tried again (the missing list is cleared), or 0. */
    TickType_t missingRetryAt = 0;
    static constexpr uint32_t MEMORY_RETRY_MS = 5000;
    static constexpr int MAX_PENDING_WRITES = 32;
    TileKey pendingWrites[MAX_PENDING_WRITES] {};
    int pendingSourceZoom[MAX_PENDING_WRITES] {};
    int pendingWriteCount = 0;
    static constexpr int MAX_GAPS = 16;
    TileKey gaps[MAX_GAPS] {};
    int gapCount = 0;
    Conversion conversions[MAX_CONVERSIONS] {};
    static constexpr int MAX_FAILED_CONVERSIONS = 16;
    TileKey failedConversions[MAX_FAILED_CONVERSIONS] = {};
    int failedConversionNext = 0;
    int failedConversionCount = 0;
    /** Tiles whose preparation failed once and went back into the queue. */
    TileKey retriedConversions[MAX_FAILED_CONVERSIONS] = {};
    int retriedConversionNext = 0;
    int retriedConversionCount = 0;
    int conversionCount = 0;
    PreparedScanner scanner;
    ScanResult scanResult {};
    bool scanning = false;
    std::atomic<bool> rescanRequested = false;
    std::atomic<bool> deflatePrepared = true;
    std::atomic<bool> conversionsPaused = false;
    std::atomic<bool> routing = false;
    bool magnifiedOutdated = false;
    std::atomic<bool> sdCache = true;
    std::atomic<bool> idle = false;
    /** Ticks: when idle began (or a download ended), and until when conversions wait for memory. */
    std::atomic<TickType_t> idleSince = 0;
    std::atomic<TickType_t> conversionsWaitUntil = 0;
    char styleId[8] = "";
    CacheIndex cacheIndex;
    CacheWalker cacheWalker;
    bool walking = false;
    int writesSinceSave = 0;
    ByteWriter indexBytes;
};

} // namespace maps
