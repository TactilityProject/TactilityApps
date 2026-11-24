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
        /** Done because another job replaced it (or Pause): not finished, continues from its meta.json later. */
        bool paused;
        /** The job fetches heights (done and total count elevation tiles). */
        bool terrain;
        /** The job fetches marked trails (done and total count the steps: query, files). */
        bool trails;
    };

    bool start(const char* root, TileStore* store, IoTask* io);
    void stop();

    /** Starts or continues the download of a region whose meta.json exists, pausing a running job. */
    bool download(const char* region);
    /** Deletes a region, keeping the tiles another region still needs. */
    bool remove(const char* region);
    /**
     * Fetches the region's elevation (AWS Terrain Tiles, z12) and stores a height file next to each z12 tile
     * (render/TerrainOverlay.h). One request at a time, at most MAX_TERRAIN_TILES. Tiles already on the card are skipped.
     */
    bool downloadTerrain(const char* region);
    /**
     * Fetches the region's marked trails with one Overpass API query and stores a
     * .trl file next to each z12 tile (render/Trails.h). After a 429 or 504 it waits 30 s, at most twice. A region
     * with all its trails has maps/<region>/trails.ok.
     */
    bool downloadTrails(const char* region);
    /**
     * Trails follow every finished download and the browsed map (always on since 23 Nov 2026; drawn in the outdoor style
     * only); the regions downloaded before get theirs in idle time, one query at a time.
     */
    void setTrailsWithDownloads(bool value);
    /**
     * Asks for the trails of a z12 tile: the map drew a tile on screen there and found no trail file.
     * Fetched in idle time, after the heights, as one small query for its z11 block (4 z12 tiles) into the browsed
     * region; tiles without trails get an empty file, so they are not asked for again.
     */
    void queueTrails(const TileKey& tile);
    /**
     * Heights follow every finished download and browsed tiles (Settings, Terrain); switched on, the regions downloaded
     * before get theirs in idle time (a region with all of them has maps/<region>/terrain.ok).
     */
    void setTerrainWithDownloads(bool value);
    /** Whether height files were stored since the last call; the map looks them up again. */
    /**
     * Asks for the heights of a z12 tile (Settings, Terrain on): the map drew a tile on screen there without them.
     * Fetched in idle time into the browsed region; a tile is not asked for twice in a row.
     */
    void queueTerrain(const TileKey& tile);
    /** The z12 tiles of the files into files (MAX_TERRAIN_ARRIVED of them), count -1 when more came. */
    bool takeTerrainChanged(TileKey* files, int& count);
    static constexpr int MAX_TERRAIN_ARRIVED = 8;
    static constexpr int MAX_TERRAIN_TILES = 300;
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
    /** Whether every tile on screen is drawn: idle-time heights wait for it (drawing takes the internal RAM). */
    void setViewComplete(bool value) {
        // A view completing with terrain waiting starts it at once
        if (value && !viewComplete.exchange(true) && ((terrainWithDownloads && browseTerrainCount > 0) || (trailsWithDownloads && browseTrailsCount > 0)) &&
            wake != nullptr) {
            xSemaphoreGive(wake);
        }
        viewComplete = value;
    }
    TaskHandle_t browseTaskHandle() const { return browseTask; }
    TaskHandle_t controlTaskHandle() const { return controlTask; }
    /** Logs the phases of the first TileJSON request (developer option). */
    void setTiming(bool value) { timing = value; }

private:
    enum class Job : uint8_t {
        None,
        Download,
        Update,
        Delete,
        Terrain,
        Trails
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
        /** The largest answer taken (0: MAX_TILE_BYTES). */
        size_t maxBytes = 0;
        /**
         * Once connected, reads wait at most this long (0: the client's timeout): perform() then returns
         * ESP_ERR_HTTP_EAGAIN and goes on where it stopped when called again, so a long wait can give way.
         */
        int shortReadMs = 0;
        /** Whether the client has an open connection the next request can reuse. */
        bool connected = false;
        /** Whether this connection holds the handshake lock. */
        bool handshaking = false;
        /** This connection holds the handshake lock (taking it can time out). */
        bool handshakeLocked = false;
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
    /** Fetches the heights of the region in meta; false when it failed or was cancelled. */
    /** quiet: an idle-time run, which says nothing unless it fetched heights. */
    bool runTerrain(const RegionMeta& region, bool quiet = false);
    /** The heights of the listed z12 tiles that are not on the card yet; the list is reused for the missing ones. */
    bool runTerrainTiles(const char* regionName, TileKey* tiles, int count, bool quiet);
    /** Stores a height file (a TerrainFileJob), trying again when the write gave way to the map. */
    bool storeTerrain(void* job);
    /** Runs a card write job, trying again while it gives way to the map (as storeTerrain). */
    bool storeGivingWay(IoJob write, void* job);
    /** Idle-time heights: browsed areas, and the regions without them after Terrain was switched on. */
    void runPendingTerrain();
    /** Takes the control task for idle work when no job waits; releaseIdle() gives it back. */
    bool claimIdle();
    void releaseIdle();
    /** A region's trails; on success it writes trails.ok. Quiet: idle time, no retries, no messages but the progress. */
    bool runTrails(const RegionMeta& region, bool quiet = false);
    /**
     * One Overpass query for a box, its trail files stored into a region. markEmpty: every z12 tile of the box gets a
     * file, an empty one where no trail is (the map then knows it was asked).
     */
    bool runTrailsArea(const char* regionName, const BoundingBox& box, bool quiet, bool markEmpty);
    /** Idle-time trails: blocks of the browsed map, then the regions without trails.ok. One query per call. */
    void runPendingTrails();
    /** Heights or trail blocks of the browsed map wait and can run now: idle-time updates of regions wait for them. */
    bool screenWorkWaiting();
    /** Reads which regions have all their trails (trails.ok) and their boxes. */
    void refreshTrailRegions();
    /** Whether a region with all its trails covers the z12 tiles of a z11 block. */
    bool trailsCovered(const TileKey& block) const;
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
    /**
     * Makes sure the browsed map exists (folder, meta.json, in the store) and its box takes in these tiles, for trail
     * and height files of areas that have no browsed tiles: in a downloaded area without them, trail blocks were
     * dropped while no browsed map existed (24 Nov).
     */
    bool coverInBrowsed(const TileKey* tiles, int count);
    /** A trail block left the queue unasked: it may be queued again when the map shows it. */
    void forgetTrailBlock(const TileKey& block);
    void saveProgress();
    bool readMeta(const char* region, RegionMeta& out);
    bool writeMeta(const RegionMeta& out);
    void setStatus(State state, const char* message, bool paused = false);
    bool claimIndex(int64_t& index, int connectionIndex);
    void finishIndex(int connectionIndex);

    char rootPath[PATH_MAX_LENGTH] = "";
    TileStore* store = nullptr;
    IoTask* io = nullptr;
    SemaphoreHandle_t mutex = nullptr;
    SemaphoreHandle_t wake = nullptr;
    SemaphoreHandle_t stopped = nullptr;
    TaskHandle_t controlTask = nullptr;
    /** The last loadOutlineTiles() failed for memory, not for a missing catalog (for the status text). */
    bool outlineNoMemory = false;
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
    bool terrainWithDownloads = false;
    std::atomic<bool> terrainSweep { false };
    uint32_t terrainFailedMs = 0;
    /** When browsing last ended (ms), 0 before. */
    std::atomic<uint32_t> browseEndedAt { 0 };
    /** The last idle-time heights run stopped because map tiles were browsed. */
    bool terrainYielded = false;
    static constexpr int BROWSE_TERRAIN = 16;
    /** z12 tiles whose heights a browsed tile asked for, and the last ones queued (not asked for twice). */
    TileKey browseTerrain[BROWSE_TERRAIN] {};
    int browseTerrainCount = 0;
    TileKey browseTerrainSeen[BROWSE_TERRAIN] {};
    int browseTerrainSeenNext = 0;
    std::atomic<bool> trailsWithDownloads { false };
    /** Regions without trails.ok are looked for (Marked trails switched on, or a region's query failed). */
    std::atomic<bool> trailsSweep { false };
    /**
     * After an idle-time query failed, the next one waits: a browsed block TRAILS_BLOCK_RETRY_MS, a region
     * TRAILS_RETRY_MS (a failed region does not hold up the blocks of the screen). Queries are TRAILS_GAP_MS apart.
     */
    uint32_t trailsFailedMs = 0;
    uint32_t trailsSweepFailedMs = 0;
    uint32_t trailsQueryMs = 0;
    /** The last query found no TLS connection: the firmware cannot reach the server, not asked again this session. */
    std::atomic<bool> trailsNoTls { false };
    /** The HTTP status of the last query (0: none). */
    int trailsStatus = 0;
    /** The last quiet query gave the connection to browsing before its answer came: asked again later, no failure. */
    bool trailsYielded = false;
    static constexpr int BROWSE_TRAILS = 8;
    static constexpr int TRAILS_SEEN = 32;
    /** z11 blocks of tiles on screen without trail files, the latest last; and the last ones queued. */
    TileKey browseTrails[BROWSE_TRAILS] {};
    int browseTrailsCount = 0;
    TileKey browseTrailsSeen[TRAILS_SEEN] {};
    int browseTrailsSeenNext = 0;
    /** Boxes of the regions with trails.ok (control task only). */
    BoundingBox trailRegions[TileStore::MAX_REGIONS] {};
    int trailRegionCount = 0;
    std::atomic<bool> trailRegionsStale { true };
    std::atomic<bool> terrainChanged { false };
    /** A height or trail file was stored for this z12 tile. */
    void noteTerrainFile(const TileKey& tile);
    TileKey terrainArrived[MAX_TERRAIN_ARRIVED] {};
    int terrainArrivedCount = 0;
    bool terrainArrivedAll = false;
    ChunkedBody bodies[CONNECTIONS];
    SemaphoreHandle_t tileJsonLock = nullptr;
    /** browseMeta: the browse task grows it with tiles, the control task with trail and height blocks. */
    SemaphoreHandle_t browseMetaLock = nullptr;
    /** Held for each HTTPS request (perform()). */
    SemaphoreHandle_t transferLock = nullptr;
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
    /** Tiles that failed while browsing, not asked for again until BROWSE_FAILED_MS after the failure. */
    static constexpr int BROWSE_FAILED = 8;
    TileKey browseFailed[BROWSE_FAILED] {};
    uint32_t browseFailedMs[BROWSE_FAILED] {};
    int browseFailedNext = 0;
    std::atomic<bool> browsing = false;
    std::atomic<bool> idle = true;
    std::atomic<bool> viewComplete = false;
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
