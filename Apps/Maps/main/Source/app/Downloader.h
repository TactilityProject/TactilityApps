// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include "../render/PreparedTile.h"
#include "../tiles/RegionMeta.h"
#include "../tiles/TileStore.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace maps {

/**
 * Downloads the tiles of a region from OpenFreeMap in the background, and deletes regions.
 * The tile URL comes from OpenFreeMap's TileJSON. Two connections run at the same time, each kept alive
 * across tiles. A tile is received into PSRAM and written by the I/O task, after the tiles on screen.
 * Progress is saved in the region's meta.json, so a download continues after a restart.
 *
 * While browsing, a third connection fetches the tiles on screen that have no detail on the card yet into the
 * region "browsed", one tile at a time and only while Wi-Fi is connected.
 */
class Downloader {
public:
    static constexpr int CONNECTIONS = 2;

    enum class State : uint8_t {
        Idle,
        Connecting,
        Downloading,
        Deleting,
        Done,
        Failed
    };

    struct Status {
        State state;
        char region[REGION_NAME_MAX + 1];
        int64_t done;
        int64_t total;
        uint64_t bytes;
        char message[64];
    };

    bool start(const char* root, TileStore* store, IoTask* io);
    void stop();

    /** Starts or continues the download of a region whose meta.json exists, pausing a running job. */
    bool download(const char* region);
    /** Deletes a region, keeping the tiles another region still needs. */
    bool remove(const char* region);
    void cancel() { cancelRequested = true; }

    Status status();
    bool isBusy() const { return busy.load(); }
    /** The tile set version OpenFreeMap serves now, empty until a download asked for it. */
    const char* latestVersion() const { return version; }
    /** True once after tiles were added to or removed from the card. */
    bool takeTilesChanged() { return tilesChanged.exchange(false); }

    /** Fetches these tiles soon, after the ones already asked for, unless the server had no data for them. */
    void browse(const TileKey* tiles, int count);
    /**
     * Asks OpenFreeMap for its TileJSON now, while Wi-Fi is up. The first connection of a session pauses the
     * device for about 1.5 s, better right after the first view than when the user pans into new ground.
     */
    void prepareBrowsing();
    /** Whether tiles for the screen are being fetched. */
    bool isBrowsing() const { return browsing.load(); }
    static bool isOnline();
    /** Idle: no input for a while. Downloaded tiles are only written to the card while idle. */
    void setIdle(bool value) { idle = value; }
    TaskHandle_t browseTaskHandle() const { return browseTask; }
    TaskHandle_t controlTaskHandle() const { return controlTask; }
    /** Logs the phases of the first TileJSON request (developer option). */
    void setTiming(bool value) { timing = value; }

private:
    enum class Job : uint8_t {
        None,
        Download,
        Delete
    };

    struct Connection {
        Downloader* owner;
        int index;
        void* client;
        ByteWriter* body;
        uint64_t received;
        TaskHandle_t task;
        char lastFolder[PATH_MAX_LENGTH];
    };

    bool queue(Job next, const char* region);
    static void controlMain(void* context);
    static void connectionMain(void* context);
    void controlLoop();
    void runDownload();
    void runDelete();
    void connectionLoop(Connection& connection);
    bool fetchTileJson();
    bool downloadTile(Connection& connection, const char* region, const TileKey& tile, int& status);
    bool tileOnCard(const char* region, const TileKey& tile, bool& inThisRegion);
    bool ensureTileJson();
    static void browseMain(void* context);
    void browseLoop();
    bool fetchBrowseTile(const TileKey& tile);
    bool ensureBrowseRegion(const TileKey& tile);
    void saveProgress();
    bool readMeta(const char* region, RegionMeta& out);
    bool writeMeta(const RegionMeta& out);
    void setStatus(State state, const char* message);
    bool claimIndex(int64_t& index, int connectionIndex);
    void finishIndex(int connectionIndex);

    char rootPath[PATH_MAX_LENGTH] = "";
    TileStore* store = nullptr;
    IoTask* io = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t wake = nullptr;
    SemaphoreHandle_t stopped = nullptr;
    TaskHandle_t controlTask = nullptr;
    std::atomic<bool> stopRequested = false;
    std::atomic<bool> cancelRequested = false;
    std::atomic<bool> busy = false;
    std::atomic<bool> tilesChanged = false;
    Job job = Job::None;
    char jobRegion[REGION_NAME_MAX + 1] = "";
    Status current {};
    RegionMeta meta {};
    char urlTemplate[160] = "";
    char version[48] = "";
    Connection connections[CONNECTIONS] {};
    ByteWriter bodies[CONNECTIONS];
    SemaphoreHandle_t tileJsonLock = nullptr;
    SemaphoreHandle_t browseWake = nullptr;
    SemaphoreHandle_t browseStopped = nullptr;
    TaskHandle_t browseTask = nullptr;
    static constexpr int BROWSE_QUEUE = 16;
    static constexpr int BROWSE_EMPTY = 64;
    TileKey browseQueue[BROWSE_QUEUE] {};
    int browseCount = 0;
    /** Tiles the server had no data for, the sea or empty land, so they are not asked for again. */
    TileKey browseEmpty[BROWSE_EMPTY] {};
    int browseEmptyCount = 0;
    int browseEmptyNext = 0;
    std::atomic<bool> browsing = false;
    std::atomic<bool> idle = true;
    std::atomic<bool> warmUp = false;
    bool timing = false;
    Connection browseConnection {};
    ByteWriter browseBody;
    RegionMeta browseMeta {};
    bool browseMetaReady = false;
    int browseUnsaved = 0;
    SemaphoreHandle_t connectionsDone = nullptr;
    int64_t nextIndex = 0;
    int64_t activeIndex[CONNECTIONS] {};
    int64_t sinceSave = 0;
    std::atomic<bool> failed = false;
};

} // namespace maps
