// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include "../mvt/ByteInput.h"
#include "../render/CacheIndex.h"
#include "../render/CacheWalker.h"
#include "../render/PreparedScanner.h"
#include "../render/PreparedTile.h"
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
 * so they never delay a tile on screen. In idle time the lanes also convert originals into prepared tiles,
 * the ones just drawn from an original first, then every tile on the card found by a PreparedScanner.
 */
class TileWorker {
public:
    static constexpr int MAX_WANTED = 32;
    static constexpr int LANES = 2;

    bool start(TileStore* store, IoTask* io, const Palette* palette, int cacheSlots, const char* cacheRoot);
    void stop();

    /** Replaces the wanted tiles. The first visibleCount are on screen, the rest are fetched ahead. */
    void want(const TileKey* tiles, int count, int visibleCount);

    /** The cache is shared with the render tasks, so every access happens between lock() and unlock(). */
    void lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
    void unlock() { xSemaphoreGive(mutex); }
    TileCache& cache() { return tileCache; }

    /** True once since the last call when a tile was added to the cache. */
    bool takeChanged() { return changed.exchange(false); }
    bool isBusy() const { return busyLanes.load() > 0; }
    TaskHandle_t laneTask(int lane) const { return lane < laneCount ? lanes[lane].task : nullptr; }
    /** The fewest bytes of stack a render lane had left so far. */
    uint32_t stackLeft(int lane) const {
        return lane < laneCount && lanes[lane].task != nullptr ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(lanes[lane].task)) : 0;
    }
    /** Whether a tile is cached or known to have no data. Call between lock() and unlock(). */
    bool isSettled(const TileKey& tile) const;
    /** New vector tiles are on the card: tiles without data and tiles drawn from lower zoom levels are drawn again. */
    void tilesArrived();
    /** Whether new prepared tiles are written deflated. */
    void setDeflatePrepared(bool deflate) { deflatePrepared = deflate; }
    /** Whether rendered tiles are read from and written to the SD card cache. */
    void setSdCache(bool enabled) { sdCache = enabled; }
    /** Idle: no input for a while, so prefetch, conversions and cache tidying may run. */
    void setIdle(bool value) { idle = value; }
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
        TaskHandle_t task;
        TileKey current;
        bool active;
        uint16_t* writeBuffer;
        uint16_t* encodeBuffer;
        TileConverter converter;
        ByteWriter converted;
        bool converting;
        TileKey convertingTile;
        bool prefetching;
        std::atomic<bool> cancel;
    };

    struct Conversion {
        TileKey tile;
        int region;
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

    struct LoadJob {
        TileWorker* worker;
        Lane* lane;
        TileKey tile;
        bool tryCache;
        bool skipPrepared;
        Found found;
        TileFile source;
        int cachedSourceZoom;
        bool yielded;
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
    bool queueConversion(const TileKey& tile, int region);
    static bool collectFromScan(const TileKey& tile, int region, void* context);
    /** Converts one queued tile, or looks at one more folder for tiles to convert. Returns false when there is nothing to do. */
    bool convertOne(Lane& lane);
    /** Deletes the least recently used SD cache tile while the cache is over its cap, or tidies one cache folder. */
    bool tidyCache();
    void saveCacheIndex();
    static void lockIndex(void* context);
    static void unlockIndex(void* context);

    TileStore* store = nullptr;
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
    static constexpr int MAX_PENDING_WRITES = 32;
    TileKey pendingWrites[MAX_PENDING_WRITES] {};
    int pendingSourceZoom[MAX_PENDING_WRITES] {};
    int pendingWriteCount = 0;
    static constexpr int MAX_GAPS = 16;
    TileKey gaps[MAX_GAPS] {};
    int gapCount = 0;
    Conversion conversions[MAX_CONVERSIONS] {};
    int conversionCount = 0;
    PreparedScanner scanner;
    ScanResult scanResult {};
    bool scanning = false;
    std::atomic<bool> rescanRequested = false;
    std::atomic<bool> deflatePrepared = true;
    std::atomic<bool> sdCache = true;
    std::atomic<bool> idle = false;
    char styleId[8] = "";
    CacheIndex cacheIndex;
    CacheWalker cacheWalker;
    bool walking = false;
    int writesSinceSave = 0;
    ByteWriter indexBytes;
};

} // namespace maps
