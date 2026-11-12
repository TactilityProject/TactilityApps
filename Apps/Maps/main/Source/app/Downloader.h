// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include "../render/PreparedTile.h"
#include "../tiles/BrowsedMove.h"
#include "../tiles/ChunkedBody.h"
#include "../tiles/RegionMeta.h"
#include "../tiles/TileStore.h"

#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdint>

namespace maps {

/**
 * Downloads the tiles of a region from OpenFreeMap in the background, and deletes regions.
 * A tile the user already browsed is moved from the region "browsed" into the downloading region instead.
 * The tile URL comes from OpenFreeMap's TileJSON. CONNECTIONS connections (one, see there) run at the same time,
 * each kept alive across tiles. A tile is received into PSRAM and written by the I/O task, after the tiles on screen.
 * Progress is saved in the region's meta.json, so a download continues after a restart.
 *
 * While browsing, a third connection fetches the tiles on screen that have no detail on the card yet into the
 * region "browsed", one tile at a time and only while Wi-Fi is connected.
 *
 * A TLS handshake costs 2-3.6 s of CPU (the firmware cannot resume sessions), so connections are pooled: a
 * finished download or browse run leaves its connection open for the next request of any kind, and one left
 * unused for 60 s is closed. New connections are opened one at a time.
 *
 * Regions whose tiles come from an older OpenFreeMap version are updated while the map is left alone: one tile at a
 * time over one connection, the tiles in view first, progress kept in the region's update.bin, any input pauses it.
 */
class Downloader {
public:
    /**
     * One download connection. Measured on the T-Deck (10 Nov 2026, the same 38 tiles twice each): two connections took
     * 31-35 s with 3-4 TLS reads failing for internal RAM (esp-aes) and a 2 s reconnect each, one took 33 s with none,
     * small tiles 250 ms each instead of 780 ms, and kept 9 KB more internal RAM free.
     */
    static constexpr int CONNECTIONS = 1;
    /**
     * Network tasks run on core 1. While a TLS handshake (2-3 s of CPU) ran on core 0, the map loop and the render
     * lane there made no progress for its whole length (docs/upstream/https-stall.md).
     */
    static constexpr BaseType_t NETWORK_CORE = 1;
    /** Whether a TLS handshake is running on the network core now; the render lane there waits for it. */
    static bool isHandshaking() { return handshakes.load() > 0; }

    enum class State : uint8_t {
        Idle,
        Connecting,
        Downloading,
        Updating,
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
    /** Brings a region to the newest tile version now, without waiting for idle time (Regions, Update now). */
    bool update(const char* region);
    /**
     * Whether idle-time updates may run: the map was left alone long enough, Wi-Fi and power allow it. Set to false on
     * any input, a running idle update stops after the tile it is fetching and continues later.
     */
    void setUpdatesAllowed(bool value) { updatesAllowed = value; }
    /** The area on screen, so updates fetch the tiles the user sees first. */
    void setView(const BoundingBox& box);
    /** The region catalog file (in the app's assets), for regions that follow a catalog outline. */
    void setCatalogPath(const char* path);
    /** Asked for a free PSRAM block of bytes when a large tile does not fit; returns whether it made one. */
    using MemoryRelief = bool (*)(void* context, size_t bytes);
    void setMemoryRelief(MemoryRelief relief, void* context) {
        memoryRelief = relief;
        memoryReliefContext = context;
    }
    void cancel() { cancelRequested = true; }

    Status status();
    bool isBusy() const { return busy.load(); }
    /** The tile set version OpenFreeMap serves now, empty until a download asked for it. */
    const char* latestVersion() const { return version; }
    /** True once after tiles were added to or removed from the card. */
    /**
     * Whether tiles were stored or removed since the last call. count is the number of tiles in tiles (fetched while
     * browsing, at most MAX_ARRIVED), or -1 when any tile may have changed.
     */
    bool takeTilesChanged(TileKey* tiles, int& count);
    static constexpr int MAX_ARRIVED = 8;

    /** Fetches these tiles soon, after the ones already asked for, unless the server had no data for them. */
    void browse(const TileKey* tiles, int count);
    /**
     * Drops queued browse tiles the view no longer needs: those at another zoom than sourceZoom or outside columns
     * minX..maxX (a range that wraps when minX > maxX) and rows minY..maxY. The tile being fetched is left to finish.
     */
    void keepBrowse(int sourceZoom, int minX, int minY, int maxX, int maxY);
    /**
     * Asks OpenFreeMap for its TileJSON now, while Wi-Fi is up. The first connection of a session pauses the
     * device for about 1.5 s, better right after the first view than when the user pans into new ground.
     */
    void prepareBrowsing();
    /** Whether tiles for the screen are being fetched. */
    bool isBrowsing() const { return browsing.load(); }
    /** Tiles fetched and asked for since the browse queue was last empty, for "Fetching detail 2/4". */
    void browseProgress(int& done, int& total);
    static bool isOnline();
    /** Browsed tiles moved into the region the current download fills, so far in this run. */
    int movedTiles() const;
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
        Update,
        Delete
    };

    struct Connection {
        Downloader* owner;
        int index;
        void* client;
        ChunkedBody* body;
        uint64_t received;
        TaskHandle_t task;
        char lastFolder[PATH_MAX_LENGTH];
        /** When the request now running started, and how long its TCP and TLS connect took (0: reused). */
        uint32_t requestStartMs = 0;
        uint32_t connectMs = 0;
        /** Whether the client has an open connection the next request can reuse. */
        bool connected = false;
        /** Whether this connection holds the handshake lock. */
        bool handshaking = false;
        /** Tiles moved from the browsed region instead of downloaded in this run, and their bytes. */
        int movedTiles = 0;
        uint64_t moved = 0;
        /** Bytes of stale browsed copies removed after a fresh download replaced them. */
        uint64_t browsedRemoved = 0;
    };

    /** What the card has of a tile a download wants. */
    enum class OnCard : uint8_t { Missing, InRegion, Shared, InBrowsed };
    OnCard findTile(const char* region, const TileKey& tile);
    /** Moves a browsed tile into the region; Stale when the browsed copy is from another source version. */
    BrowsedMove moveBrowsedTile(Connection& connection, const TileKey& tile, uint64_t& size);
    void removeBrowsedCopy(Connection& connection, const TileKey& tile);
    /** Takes the bytes of moved and removed tiles off the browsed region's meta.json. */
    void settleBrowsed(uint64_t removedBytes);

    struct IdleClient {
        void* client;
        uint32_t sinceMs;
    };

    static esp_err_t onHttpEvent(esp_http_client_event_t* event);
    /** A keep-alive HTTPS client for OpenFreeMap whose events go to connection. */
    static void* createClient(Connection* connection);
    /** Runs the request set up on connection's client, and logs the TLS connection if it had to open one. */
    esp_err_t perform(Connection& connection);
    void beginHandshake(Connection& connection);
    void endHandshake(Connection& connection);
    /** Gives connection an open pooled client, or a new one. */
    void takeClient(Connection& connection);
    /** Returns connection's client to the pool when it is still connected, otherwise closes it. */
    void giveClient(Connection& connection);
    static void dropClient(Connection& connection);
    void closeIdleClients(uint32_t maxIdleMs);
    bool queue(Job next, const char* region);
    static void controlMain(void* context);
    static void connectionMain(void* context);
    void controlLoop();
    void runDownload();
    void runDelete();
    /** Updates one region whose tiles are older than the source; manual: not paused by input. */
    void runUpdate(bool manual);
    /** The name of a complete region with an older tile version than the source, or false. */
    bool findOutdated(char* region, size_t size);
    void noteArrived(const TileKey& tile);
    /**
     * For a region with a catalog outline: the list of its tiles, zoom by zoom, row by row, made once per job from the
     * catalog so the order never changes. Without an outline the tiles come from the plan's rectangle.
     */
    bool loadOutlineTiles(const RegionMeta& region);
    void freeOutlineTiles();
    bool tileForIndex(const RegionMeta& region, int64_t index, TileKey& tile) const;
    /** The nearest city, town or village to the region's centre, from its own z12 or z10 tile. */
    bool findPlaceName(const RegionMeta& region, char* out, size_t size);
    void connectionLoop(Connection& connection);
    bool fetchTileJson(Connection& connection);
    /** waitForIdle: the tile is written only once the map is idle (downloads); tiles for the screen are written at once. */
    bool downloadTile(Connection& connection, const char* region, const TileKey& tile, int& status, bool waitForIdle = true);
    bool tileOnCard(const char* region, const TileKey& tile, bool& inThisRegion);
    bool ensureTileJson(Connection& connection);
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
    std::atomic<bool> allTilesChanged = false;
    TileKey arrived[MAX_ARRIVED] {};
    int arrivedCount = 0;
    Job job = Job::None;
    char jobRegion[REGION_NAME_MAX + 1] = "";
    Status current {};
    RegionMeta meta {};
    char urlTemplate[160] = "";
    char version[48] = "";
    Connection connections[CONNECTIONS] {};
    ChunkedBody bodies[CONNECTIONS];
    SemaphoreHandle_t tileJsonLock = nullptr;
    SemaphoreHandle_t browseWake = nullptr;
    SemaphoreHandle_t browseStopped = nullptr;
    TaskHandle_t browseTask = nullptr;
    static constexpr int BROWSE_QUEUE = 16;
    static constexpr int BROWSE_EMPTY = 64;
    TileKey browseQueue[BROWSE_QUEUE] {};
    int browseCount = 0;
    int browseDone = 0;
    int browseTotal = 0;
    /** Tiles the server had no data for, the sea or empty land, so they are not asked for again. */
    TileKey browseEmpty[BROWSE_EMPTY] {};
    int browseEmptyCount = 0;
    int browseEmptyNext = 0;
    std::atomic<bool> browsing = false;
    std::atomic<bool> idle = true;
    std::atomic<bool> warmUp = false;
    bool timing = false;
    Connection browseConnection {};
    ChunkedBody browseBody;
    MemoryRelief memoryRelief = nullptr;
    void* memoryReliefContext = nullptr;
    RegionMeta browseMeta {};
    bool browseMetaReady = false;
    int browseUnsaved = 0;
    SemaphoreHandle_t connectionsDone = nullptr;
    int64_t nextIndex = 0;
    int64_t activeIndex[CONNECTIONS] {};
    int64_t sinceSave = 0;
    std::atomic<bool> failed = false;
    static constexpr int IDLE_CLIENTS = CONNECTIONS + 1;
    IdleClient idleClients[IDLE_CLIENTS] {};
    int idleCount = 0;
    SemaphoreHandle_t handshakeLock = nullptr;
    static std::atomic<int> handshakes;
    std::atomic<bool> updatesAllowed = false;
    BoundingBox viewBox {};
    bool viewKnown = false;
    uint32_t lastUpdateCheckMs = 0;
    char catalogPath[PATH_MAX_LENGTH] = "";
    TileKey* outlineTiles = nullptr;
    int64_t outlineCount = 0;
};

} // namespace maps
