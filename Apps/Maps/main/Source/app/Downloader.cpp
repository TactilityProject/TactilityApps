// SPDX-License-Identifier: GPL-3.0-or-later
#include "Downloader.h"
#include "RootCertificates.h"

#include "../platform/Platform.h"
#include "../render/TerrainOverlay.h"
#include "../render/Trails.h"
#include "../mvt/PlaceFinder.h"
#include "../tiles/BrowsedMove.h"
#include "../tiles/RegionCatalog.h"
#include "../tiles/TileJson.h"

#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <tactility/device.h>
#include <tactility/drivers/wifi.h>

#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <new>

namespace maps {

namespace {

constexpr uint32_t CONTROL_STACK = 8192;
constexpr uint32_t CONNECTION_STACK = 6144;
constexpr UBaseType_t TASK_PRIORITY = 3;
/** Tiles at this zoom level and below are downloaded one at a time. */
constexpr int ALONE_MAX_ZOOM = 7;
/** Idle keep-alive connections are closed after this long, a new one costs a full TLS handshake. */
constexpr uint32_t IDLE_CLOSE_MS = 60000;
/** After heights failed in idle time, the next try waits this long. */
constexpr uint32_t TERRAIN_RETRY_MS = 120000;
/** Idle-time heights start only with an internal RAM block this large free (they worked with 10 KB, failed with 2.9 KB). */
constexpr size_t TERRAIN_INTERNAL_BLOCK = 8 * 1024;
/** How often the network tasks look for idle connections to close. */
constexpr TickType_t SWEEP_TICKS = pdMS_TO_TICKS(5000);
/** How often waiting heights are looked at. */
constexpr TickType_t TERRAIN_POLL_TICKS = pdMS_TO_TICKS(500);

/**
 * Starts a network task with its stack in PSRAM, which keeps about 28 KB of internal RAM free for Wi-Fi, TLS and
 * the display. These tasks never touch flash or the SD card themselves. The task ends by suspending itself after
 * signalling its owner, who then deletes it with endNetworkTask().
 */
bool startNetworkTask(TaskFunction_t function, const char* name, uint32_t stack, void* context, TaskHandle_t* task) {
    return xTaskCreatePinnedToCoreWithCaps(function, name, stack, context, TASK_PRIORITY, task, Downloader::NETWORK_CORE, MALLOC_CAP_SPIRAM) == pdPASS;
}

void endNetworkTask(TaskHandle_t task) {
    // It signalled before suspending itself, wait until it got there
    TaskStatus_t status {};
    for (int i = 0; i < 100; i++) {
        vTaskGetInfo(task, &status, pdFALSE, eInvalid);
        if (status.eCurrentState == eSuspended) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDeleteWithCaps(task);
}
constexpr int HTTP_TIMEOUT_MS = 15000;
constexpr int SAVE_EVERY_TILES = 20;
/** How often idle time looks for regions that need an update. */
constexpr uint32_t UPDATE_CHECK_MS = 15000;
constexpr int MAX_ATTEMPTS = 3;

bool wifiConnected() {
    Device* wifi = nullptr;
    if (device_get_first_active_by_type(&WIFI_TYPE, &wifi) != ERROR_NONE) {
        return false;
    }
    WifiStationState state = WIFI_STATION_STATE_DISCONNECTED;
    const bool connected = wifi_get_station_state(wifi, &state) == ERROR_NONE && state == WIFI_STATION_STATE_CONNECTED;
    device_put(wifi);
    return connected;
}

/** Larger responses are refused, no real tile comes close. */
constexpr size_t MAX_TILE_BYTES = 4 * 1024 * 1024;

/**
 * When the last connection was established (TCP and TLS done), in ms since boot, for the timing log. 32 bits:
 * 64-bit atomics are not exported to apps on the ESP32-S3.
 */
std::atomic<uint32_t> connectedAtMs = 0;

uint32_t msSince(int64_t startUs, int64_t endUs) {
    return static_cast<uint32_t>((endUs - startUs) / 1000);
}

/** TLS connections opened in this app session and the time they took, for the log. */
std::atomic<uint32_t> handshakeCount = 0;
std::atomic<uint32_t> handshakeTotalMs = 0;

} // namespace

std::atomic<int> Downloader::handshakes = 0;

esp_err_t Downloader::onHttpEvent(esp_http_client_event_t* event) {
    auto* connection = static_cast<Connection*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        connectedAtMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        if (connection != nullptr) {
            connection->connectMs = connectedAtMs - connection->requestStartMs;
            connection->connected = true;
            if (connection->owner != nullptr) {
                connection->owner->endHandshake(*connection);
            }
        }
    }
    if (event->event_id == HTTP_EVENT_DISCONNECTED && connection != nullptr) {
        connection->connected = false;
    }
    ChunkedBody* body = connection != nullptr ? connection->body : nullptr;
    // Low-zoom tiles are up to about 1.2 MB, kept in 64 KB pieces: late in a session PSRAM has the room, but not in
    // one block
    if (event->event_id == HTTP_EVENT_ON_DATA && body != nullptr) {
        const size_t total = body->size() + static_cast<size_t>(event->data_len);
        if (total > MAX_TILE_BYTES) {
            body->fail();
        } else {
            Downloader* owner = connection->owner;
            if (!body->failed() && !body->ensure(total) && owner != nullptr && owner->memoryRelief != nullptr) {
                // No room even for a piece: the map gives some of its tile cache for it
                owner->memoryRelief(owner->memoryReliefContext, ChunkedBody::CHUNK_BYTES);
            }
            body->putBytes(event->data, static_cast<size_t>(event->data_len));
        }
    }
    return ESP_OK;
}

void* Downloader::createClient(Connection* connection) {
    esp_http_client_config_t config = {};
    config.url = TILEJSON_URL;
    config.user_agent = USER_AGENT;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.keep_alive_enable = true;
    config.cert_pem = ROOT_CERTIFICATES;
    config.cert_len = sizeof(ROOT_CERTIFICATES);
    config.event_handler = onHttpEvent;
    config.user_data = connection;
    config.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != nullptr) {
        // Tiles are stored as the server compressed them
        esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    }
    if (connection != nullptr) {
        connection->connected = false;
    }
    return client;
}

void Downloader::beginHandshake(Connection& connection) {
    // One handshake at a time: each is 2-3.6 s of CPU on the network core, two at once kept its idle task from the
    // task watchdog
    // Not taken in time (another handshake hangs): go on without it, and do not give it back
    connection.handshakeLocked = xSemaphoreTake(handshakeLock, pdMS_TO_TICKS(HTTP_TIMEOUT_MS)) == pdTRUE;
    connection.handshaking = true;
    handshakes++;
}

void Downloader::endHandshake(Connection& connection) {
    if (connection.handshaking) {
        connection.handshaking = false;
        handshakes--;
        if (connection.handshakeLocked) {
            connection.handshakeLocked = false;
            xSemaphoreGive(handshakeLock);
        }
    }
}

esp_err_t Downloader::perform(Connection& connection) {
    auto* client = static_cast<esp_http_client_handle_t>(connection.client);
    // One HTTPS transfer at a time (tiles, browsing, heights): two at once found no internal RAM for the AES buffers
    // on the T-Deck and both broke off ("esp-aes: Failed to allocate memory"). Not taken in time: go on without it.
    const bool transferLocked = xSemaphoreTake(transferLock, pdMS_TO_TICKS(HTTP_TIMEOUT_MS)) == pdTRUE;
    const bool wasConnected = connection.connected;
    if (!wasConnected) {
        beginHandshake(connection);
    }
    connection.requestStartMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    connection.connectMs = 0;
    const esp_err_t result = esp_http_client_perform(client);
    endHandshake(connection);
    if (transferLocked) {
        xSemaphoreGive(transferLock);
    }
    if (connection.connectMs > 0) {
        const uint32_t count = ++handshakeCount;
        const uint32_t total = handshakeTotalMs += connection.connectMs;
        logInfo("TLS: [%d] new connection in %u ms (session: %u connections, %u ms)", connection.index,
            static_cast<unsigned>(connection.connectMs), static_cast<unsigned>(count), static_cast<unsigned>(total));
    }
    return result;
}

void Downloader::takeClient(Connection& connection) {
    if (connection.client != nullptr) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (idleCount > 0) {
        // The most recently used one, the least likely to have been closed by the server
        idleCount--;
        connection.client = idleClients[idleCount].client;
        connection.connected = true;
    }
    xSemaphoreGive(mutex);
    if (connection.client != nullptr) {
        esp_http_client_set_user_data(static_cast<esp_http_client_handle_t>(connection.client), &connection);
    } else {
        connection.client = createClient(&connection);
    }
}

void Downloader::dropClient(Connection& connection) {
    if (connection.client != nullptr) {
        esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(connection.client));
        connection.client = nullptr;
    }
    connection.connected = false;
}

void Downloader::giveClient(Connection& connection) {
    if (connection.client == nullptr) {
        return;
    }
    if (!connection.connected) {
        dropClient(connection);
        return;
    }
    esp_http_client_set_user_data(static_cast<esp_http_client_handle_t>(connection.client), nullptr);
    xSemaphoreTake(mutex, portMAX_DELAY);
    void* surplus = nullptr;
    if (idleCount < IDLE_CLIENTS) {
        idleClients[idleCount++] = { connection.client, nowMs() };
    } else {
        surplus = connection.client;
    }
    xSemaphoreGive(mutex);
    if (surplus != nullptr) {
        esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(surplus));
    }
    connection.client = nullptr;
    connection.connected = false;
}

void Downloader::closeIdleClients(uint32_t maxIdleMs) {
    void* closing[IDLE_CLIENTS] {};
    int closingCount = 0;
    xSemaphoreTake(mutex, portMAX_DELAY);
    const uint32_t now = nowMs();
    int kept = 0;
    for (int i = 0; i < idleCount; i++) {
        if (now - idleClients[i].sinceMs >= maxIdleMs) {
            closing[closingCount++] = idleClients[i].client;
        } else {
            idleClients[kept++] = idleClients[i];
        }
    }
    idleCount = kept;
    xSemaphoreGive(mutex);
    for (int i = 0; i < closingCount; i++) {
        esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(closing[i]));
    }
    if (closingCount > 0) {
        logInfo("TLS: closed %d idle connection(s)", closingCount);
    }
}

namespace {

void makeFolders(const char* root, const char* region, const TileKey& tile) {
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/maps/%s/%d", root, region, tile.z);
    mkdir(path, 0777);
    if (tileFolderPath(path, sizeof(path), root, region, tile)) {
        mkdir(path, 0777);
    }
}

struct MetaJob {
    char path[PATH_MAX_LENGTH];
    RegionMeta* meta;
};

bool readMetaJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<MetaJob*>(context);
    return readRegionMeta(job->path, *job->meta);
}

bool writeMetaJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<MetaJob*>(context);
    return writeRegionMeta(job->path, *job->meta);
}

struct OnCardJob {
    const char* root;
    const TileStore* store;
    const char* region;
    TileKey tile;
    bool inThisRegion;
    bool inBrowsed;
};

bool onCardJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<OnCardJob*>(context);
    char path[PATH_MAX_LENGTH];
    job->inThisRegion = tilePath(path, sizeof(path), job->root, job->region, job->tile) && fileExists(path);
    if (job->inThisRegion) {
        return true;
    }
    // A tile of an overlapping region is shared instead of downloaded again. Browsed tiles are not shared with a
    // download: they move into the region (Downloader::moveBrowsedTile), so the browsed region stays a cache
    const bool forBrowsing = strcmp(job->region, BROWSE_REGION) == 0;
    for (int i = 0; i < job->store->regionCount(); i++) {
        const char* other = job->store->regionName(i);
        if (strcmp(other, job->region) == 0 || (!forBrowsing && strcmp(other, BROWSE_REGION) == 0)) {
            continue;
        }
        if (tilePath(path, sizeof(path), job->root, other, job->tile) && fileExists(path)) {
            return true;
        }
    }
    job->inBrowsed = !forBrowsing && tilePath(path, sizeof(path), job->root, BROWSE_REGION, job->tile) && fileExists(path);
    return false;
}

bool makeFolder(const char* path) {
    return mkdir(path, 0777) == 0;
}

/** Moves a browsed tile into a region (tiles/BrowsedMove.h), on the I/O task. */
struct MoveJob {
    const char* root;
    const TileStore* store;
    const char* region;
    TileKey tile;
    /** The source version the region downloads from. */
    const char* version;
    BrowsedMove result;
    uint64_t size;
};

bool moveJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<MoveJob*>(context);
    const char* browsedVersion = "";
    for (int i = 0; i < job->store->regionCount(); i++) {
        if (strcmp(job->store->regionName(i), BROWSE_REGION) == 0 && job->store->regionVersion(i) != nullptr) {
            browsedVersion = job->store->regionVersion(i);
        }
    }
    job->result = moveBrowsedFiles(job->root, job->region, job->tile, job->version, browsedVersion, job->size, makeFolder);
    return true;
}

struct RemoveBrowsedJob {
    const char* root;
    TileKey tile;
    uint64_t size;
};

bool removeBrowsedJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<RemoveBrowsedJob*>(context);
    job->size = removeBrowsedFiles(job->root, job->tile);
    return true;
}

struct StoreTileJob {
    const char* root;
    const char* region;
    TileKey tile;
    const ChunkedBody* body;
    /** The tile column folder this connection made last, so it is not made again for every tile. */
    char* lastFolder;
    bool yielded;
};

bool storeTileJob(IoTask& io, void* context) {
    auto* job = static_cast<StoreTileJob*>(context);
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH + 8];
    char prepared[PATH_MAX_LENGTH];
    if (!tilePath(path, sizeof(path), job->root, job->region, job->tile)) {
        return false;
    }
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    char folder[PATH_MAX_LENGTH];
    if (tileFolderPath(folder, sizeof(folder), job->root, job->region, job->tile) && strcmp(folder, job->lastFolder) != 0) {
        makeFolders(job->root, job->region, job->tile);
        snprintf(job->lastFolder, PATH_MAX_LENGTH, "%s", folder);
    }
    // Only tiles that are not on the card are fetched, a prepared copy can only be left from a deleted original
    for (int file = 0; file < PREPARED_FILE_COUNT; file++) {
        if (preparedFilePath(prepared, sizeof(prepared), job->root, job->region, job->tile, static_cast<PreparedFile>(file)) &&
            fileExists(prepared)) {
            remove(prepared);
        }
    }
    // A write of a few hundred KB gives way to tiles on screen, the tile stays in memory and is written again
    const bool ok = io.writeFile(path, temporary, *job->body, true);
    job->yielded = io.yielded();
    return ok;
}

constexpr int MAX_DELETE_COLUMNS = 4096;

struct ColumnsJob {
    char folder[PATH_MAX_LENGTH];
    int* columns;
    int count;
};

bool listColumnsJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<ColumnsJob*>(context);
    job->count = 0;
    return listDirectory(job->folder, [](const char* name, bool isDirectory, void* data) {
        auto* columnsJob = static_cast<ColumnsJob*>(data);
        if (isDirectory && name[0] >= '0' && name[0] <= '9' && columnsJob->count < MAX_DELETE_COLUMNS) {
            columnsJob->columns[columnsJob->count++] = atoi(name);
        }
        return true;
    }, job);
}

struct RemoveJob {
    char path[PATH_MAX_LENGTH];
    bool folder;
};

bool removeJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<RemoveJob*>(context);
    return (job->folder ? rmdir(job->path) : remove(job->path)) == 0;
}

constexpr int BATCH_SIZE = 16;

/** Removes the files of a folder (not its subfolders), a few at a time so no name list can overflow. */
bool clearFolderJob(IoTask& io, void* context) {
    (void)io;
    const char* folder = static_cast<const RemoveJob*>(context)->path;
    struct Batch {
        char names[BATCH_SIZE][24];
        int count;
    };
    Batch batch {};
    for (;;) {
        batch.count = 0;
        listDirectory(folder, [](const char* name, bool isDirectory, void* data) {
            auto* batch = static_cast<Batch*>(data);
            if (!isDirectory && strlen(name) < sizeof(batch->names[0])) {
                snprintf(batch->names[batch->count++], sizeof(batch->names[0]), "%s", name);
            }
            return batch->count < BATCH_SIZE;
        }, &batch);
        int removed = 0;
        for (int i = 0; i < batch.count; i++) {
            char path[PATH_MAX_LENGTH];
            snprintf(path, sizeof(path), "%s/%s", folder, batch.names[i]);
            if (remove(path) == 0) {
                removed++;
            }
        }
        // A full batch may have more behind it; stop when nothing could be removed
        if (batch.count < BATCH_SIZE || removed == 0) {
            return true;
        }
    }
}

} // namespace

bool Downloader::start(const char* root, TileStore* tileStore, IoTask* ioTask) {
    snprintf(rootPath, sizeof(rootPath), "%s", root);
    store = tileStore;
    io = ioTask;
    mutex = xSemaphoreCreateMutex();
    wake = xSemaphoreCreateBinary();
    stopped = xSemaphoreCreateBinary();
    connectionsDone = xSemaphoreCreateCounting(CONNECTIONS, 0);
    tileJsonLock = xSemaphoreCreateMutex();
    browseWake = xSemaphoreCreateBinary();
    browseStopped = xSemaphoreCreateBinary();
    handshakeLock = xSemaphoreCreateBinary();
    if (handshakeLock != nullptr) {
        xSemaphoreGive(handshakeLock);
    }
    transferLock = xSemaphoreCreateBinary();
    if (transferLock != nullptr) {
        xSemaphoreGive(transferLock);
    }
    if (handshakeLock == nullptr || transferLock == nullptr || mutex == nullptr || wake == nullptr || stopped == nullptr || connectionsDone == nullptr || tileJsonLock == nullptr ||
        browseWake == nullptr || browseStopped == nullptr) {
        return false;
    }
    current = {};
    current.state = State::Idle;
    stopRequested = false;
    if (!startNetworkTask(controlMain, "maps-download", CONTROL_STACK, this, &controlTask)) {
        controlTask = nullptr;
        return false;
    }
    if (!startNetworkTask(browseMain, "maps-browse", CONTROL_STACK, this, &browseTask)) {
        browseTask = nullptr;
        logError("Browse task not started");
    }
    return true;
}

void Downloader::stop() {
    if (controlTask != nullptr) {
        stopRequested = true;
        cancelRequested = true;
        xSemaphoreGive(wake);
        xSemaphoreTake(stopped, portMAX_DELAY);
        endNetworkTask(controlTask);
        controlTask = nullptr;
    }
    if (browseTask != nullptr) {
        stopRequested = true;
        xSemaphoreGive(browseWake);
        xSemaphoreTake(browseStopped, portMAX_DELAY);
        endNetworkTask(browseTask);
        browseTask = nullptr;
    }
    if (mutex != nullptr) {
        closeIdleClients(0);
    }
    for (SemaphoreHandle_t* handle : { &mutex, &wake, &stopped, &connectionsDone, &tileJsonLock, &browseWake, &browseStopped, &handshakeLock, &transferLock }) {
        if (*handle != nullptr) {
            vSemaphoreDelete(*handle);
            *handle = nullptr;
        }
    }
}

bool Downloader::queue(Job next, const char* region) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    job = next;
    snprintf(jobRegion, sizeof(jobRegion), "%s", region);
    xSemaphoreGive(mutex);
    // A running download is paused, it continues from its meta.json later
    if (busy) {
        cancelRequested = true;
    }
    busy = true;
    xSemaphoreGive(wake);
    return true;
}

bool Downloader::download(const char* region) {
    return queue(Job::Download, region);
}

bool Downloader::remove(const char* region) {
    return queue(Job::Delete, region);
}

bool Downloader::downloadTrails(const char* region) {
    return queue(Job::Trails, region);
}

bool Downloader::downloadTerrain(const char* region) {
    return queue(Job::Terrain, region);
}

bool Downloader::update(const char* region) {
    return queue(Job::Update, region);
}

void Downloader::setCatalogPath(const char* path) {
    snprintf(catalogPath, sizeof(catalogPath), "%s", path);
}

namespace {

struct CatalogJob {
    const char* path;
    FileBuffer* buffer;
};

bool readCatalogJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<CatalogJob*>(context);
    // In one go: IoTask::readFile gives way to the tiles on screen and then fails, which a background job would
    // do all the time while the map loads
    return job->buffer->load(job->path);
}

struct OutlineList {
    TileKey* tiles;
    int64_t count;
    int64_t capacity;
};

} // namespace

bool Downloader::loadOutlineTiles(const RegionMeta& region) {
    freeOutlineTiles();
    if (region.outline[0] == '\0') {
        return true;
    }
    FileBuffer file;
    CatalogJob job { catalogPath, &file };
    RegionCatalog catalog;
    if (catalogPath[0] == '\0' || !io->run(IoPriority::Background, readCatalogJob, &job) || !catalog.open(file.bytes(), file.size())) {
        // Told apart for the log: the file is there but did not fit into memory (it is read whole, 357 KB), or it is not
        uint32_t bytes = 0;
        outlineNoMemory = fileSize(catalogPath, bytes);
        if (outlineNoMemory) {
            logError("Region catalog %s: no memory for its %u bytes (largest free block %u)", catalogPath, static_cast<unsigned>(bytes),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        } else {
            logError("Region catalog %s not readable", catalogPath);
        }
        return false;
    }
    int index = -1;
    char name[sizeof(region.outline)];
    for (int i = 0; i < catalog.count() && index < 0; i++) {
        catalog.copyName(i, name, sizeof(name));
        if (strcmp(name, region.outline) == 0) {
            index = i;
        }
    }
    if (index < 0 || region.total <= 0) {
        logError("Region %s is not in the catalog", region.outline);
        return false;
    }
    OutlineList list { static_cast<TileKey*>(largeAlloc(sizeof(TileKey) * static_cast<size_t>(region.total))), 0, region.total };
    if (list.tiles == nullptr) {
        return false;
    }
    for (int z = region.minZoom; z <= region.maxZoom; z++) {
        catalog.forEachTile(index, z, [](const TileKey& tile, void* context) {
            auto* tiles = static_cast<OutlineList*>(context);
            if (tiles->count >= tiles->capacity) {
                return false;
            }
            tiles->tiles[tiles->count++] = tile;
            return true;
        }, &list);
    }
    outlineTiles = list.tiles;
    outlineCount = list.count;
    if (outlineCount != region.total) {
        logError("Region %s: %d tiles in the outline, %d expected", region.name, static_cast<int>(outlineCount), static_cast<int>(region.total));
    }
    return true;
}

namespace {

struct PlaceJob {
    char path[PATH_MAX_LENGTH];
    double fx;
    double fy;
    PlaceName place;
    bool found;
};

bool placeJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<PlaceJob*>(context);
    FileInput input;
    PlaceFinder finder;
    job->found = input.open(job->path) && finder.init() && finder.nearest(&input, job->fx, job->fy, job->place);
    return true;
}

} // namespace

bool Downloader::findPlaceName(const RegionMeta& region, char* out, size_t size) {
    const double lat = (region.box.south + region.box.north) / 2.0;
    const double lon = (region.box.west + region.box.east) / 2.0;
    // z12 shows villages, z10 only towns and cities; a village right at the centre wins over a town far away
    for (const int z : { 12, 10 }) {
        if (z < region.minZoom || z > region.maxZoom) {
            continue;
        }
        const TileKey tile = tileAt(lat, lon, z);
        PlaceJob job {};
        if (!tilePath(job.path, sizeof(job.path), rootPath, region.name, tile)) {
            continue;
        }
        job.fx = lonToWorldX(lon, z) / TILE_SIZE - tile.x;
        job.fy = latToWorldY(lat, z) / TILE_SIZE - tile.y;
        io->run(IoPriority::Background, placeJob, &job);
        if (job.found && job.place.name[0] != '\0') {
            snprintf(out, size, "%s", job.place.name);
            return true;
        }
    }
    return false;
}

void Downloader::freeOutlineTiles() {
    largeFree(outlineTiles);
    outlineTiles = nullptr;
    outlineCount = 0;
}

bool Downloader::tileForIndex(const RegionMeta& region, int64_t index, TileKey& tile) const {
    if (outlineTiles != nullptr) {
        if (index < 0 || index >= outlineCount) {
            return false;
        }
        tile = outlineTiles[index];
        return true;
    }
    return tileAtIndex(region.plan(), index, tile);
}

void Downloader::setView(const BoundingBox& box) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    viewBox = box;
    viewKnown = true;
    xSemaphoreGive(mutex);
}

Downloader::Status Downloader::status() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    Status copy = current;
    xSemaphoreGive(mutex);
    return copy;
}

void Downloader::setStatus(State state, const char* message, bool paused) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    current.state = state;
    current.paused = paused;
    snprintf(current.message, sizeof(current.message), "%s", message);
    xSemaphoreGive(mutex);
    if (state == State::Failed) {
        logError("Download: %s", message);
    } else {
        logInfo("Download: %s", message);
    }
}

void Downloader::controlMain(void* context) {
    auto* downloader = static_cast<Downloader*>(context);
    downloader->controlLoop();
    xSemaphoreGive(downloader->stopped);
    vTaskSuspend(nullptr);
}

void Downloader::controlLoop() {
    while (!stopRequested) {
        // Heights asked for by the map are looked at twice a second, so they start as soon as the map is left alone
        xSemaphoreTake(mutex, portMAX_DELAY);
        const bool heightsWaiting = browseTerrainCount > 0;
        xSemaphoreGive(mutex);
        const bool woken = xSemaphoreTake(wake, heightsWaiting ? TERRAIN_POLL_TICKS : SWEEP_TICKS) == pdTRUE;
        xSemaphoreTake(mutex, portMAX_DELAY);
        const bool jobWaiting = job != Job::None;
        xSemaphoreGive(mutex);
        if (woken && !jobWaiting && !stopRequested) {
            // Woken for terrain (browsing ended, terrain asked for, the view completed): it starts now
            if (terrainWithDownloads) {
                runPendingTerrain();
            }
            continue;
        }
        if (!woken) {
            closeIdleClients(IDLE_CLOSE_MS);
            if (!stopRequested && terrainWithDownloads) {
                runPendingTerrain();
            }
            // Outdated regions are looked for at most once a minute while the map is left alone
            if (updatesAllowed && !stopRequested && nowMs() - lastUpdateCheckMs >= UPDATE_CHECK_MS && wifiConnected()) {
                lastUpdateCheckMs = nowMs();
                char region[REGION_NAME_MAX + 1];
                if (urlTemplate[0] != '\0' && findOutdated(region, sizeof(region))) {
                    xSemaphoreTake(mutex, portMAX_DELAY);
                    const bool jobless = job == Job::None;
                    if (jobless) {
                        snprintf(current.region, sizeof(current.region), "%s", region);
                        busy = true;
                    }
                    xSemaphoreGive(mutex);
                    if (jobless) {
                        cancelRequested = false;
                        runUpdate(false);
                        xSemaphoreTake(mutex, portMAX_DELAY);
                        busy = job != Job::None;
                        xSemaphoreGive(mutex);
                        if (busy) {
                            xSemaphoreGive(wake);
                        }
                    }
                }
            }
            continue;
        }
        if (stopRequested) {
            break;
        }
        cancelRequested = false;
        xSemaphoreTake(mutex, portMAX_DELAY);
        const Job requested = job;
        job = Job::None;
        snprintf(current.region, sizeof(current.region), "%s", jobRegion);
        xSemaphoreGive(mutex);
        if (requested == Job::Download) {
            runDownload();
        } else if (requested == Job::Delete) {
            runDelete();
        } else if (requested == Job::Update) {
            runUpdate(true);
        } else if (requested == Job::Terrain || requested == Job::Trails) {
            RegionMeta region {};
            if (!readMeta(current.region, region)) {
                setStatus(State::Failed, "Region information is missing");
            } else {
                snprintf(region.name, sizeof(region.name), "%s", current.region);
                if (requested == Job::Terrain) {
                    runTerrain(region);
                } else {
                    runTrails(region);
                }
            }
        }
        xSemaphoreTake(mutex, portMAX_DELAY);
        const bool more = job != Job::None;
        busy = more;
        xSemaphoreGive(mutex);
    }
}

bool Downloader::fetchTileJson(Connection& connection) {
    if (timing) {
        // Plain HTTP first: name lookup and TCP without TLS. The HTTPS request after it then finds the name cached.
        esp_http_client_config_t config = {};
        config.url = "http://tiles.openfreemap.org/planet";
        config.user_agent = USER_AGENT;
        config.timeout_ms = HTTP_TIMEOUT_MS;
        config.event_handler = onHttpEvent;
        esp_http_client_handle_t plain = esp_http_client_init(&config);
        if (plain != nullptr) {
            const int64_t startUs = esp_timer_get_time();
            connectedAtMs = 0;
            const esp_err_t opened = esp_http_client_open(plain, 0);
            const int64_t openedUs = esp_timer_get_time();
            const int64_t headers = opened == ESP_OK ? esp_http_client_fetch_headers(plain) : -1;
            const int64_t headersUs = esp_timer_get_time();
            logInfo("Timing http: start t=%u ms, connected +%u ms (DNS and TCP), open +%u, headers +%u (%d, HTTP %d)",
                static_cast<unsigned>(startUs / 1000), connectedAtMs > 0 ? connectedAtMs - static_cast<uint32_t>(startUs / 1000) : 0, msSince(startUs, openedUs),
                msSince(startUs, headersUs), static_cast<int>(headers), esp_http_client_get_status_code(plain));
            esp_http_client_close(plain);
            esp_http_client_cleanup(plain);
        }
    }
    // Through a pooled keep-alive connection, which the tile requests after it then reuse
    takeClient(connection);
    auto* client = static_cast<esp_http_client_handle_t>(connection.client);
    if (client == nullptr || connection.body == nullptr) {
        return false;
    }
    // The TileJSON is read as plain text
    esp_http_client_delete_header(client, "Accept-Encoding");
    esp_http_client_set_url(client, TILEJSON_URL);
    connection.body->clear();
    const int64_t startUs = esp_timer_get_time();
    const esp_err_t result = perform(connection);
    const int64_t doneUs = esp_timer_get_time();
    esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    const int status = esp_http_client_get_status_code(client);
    ChunkedBody& text = *connection.body;
    bool ok = false;
    if (result != ESP_OK || status != 200 || text.failed()) {
        logError("TileJSON request failed: %s, HTTP %d", esp_err_to_name(result), status);
        dropClient(connection);
    } else {
        if (timing) {
            logInfo("Timing https: start t=%u ms, connect %u ms (DNS, TCP, TLS), whole request %u ms (%u bytes)",
                static_cast<unsigned>(startUs / 1000), static_cast<unsigned>(connection.connectMs), msSince(startUs, doneUs),
                static_cast<unsigned>(text.size()));
        }
        text.put('\0');
        ok = !text.failed() && text.contiguous() != nullptr &&
            parseTileJson(reinterpret_cast<const char*>(text.contiguous()), urlTemplate, sizeof(urlTemplate), version, sizeof(version));
        if (!ok) {
            logError("TileJSON has no usable tile URL (%u bytes read)", static_cast<unsigned>(text.size()));
        }
    }
    text.clear();
    if (ok) {
        logInfo("Tile source %s (version %s)", urlTemplate, version);
    }
    return ok;
}

bool Downloader::readMeta(const char* region, RegionMeta& out) {
    MetaJob job {};
    snprintf(job.path, sizeof(job.path), "%s/maps/%s/meta.json", rootPath, region);
    job.meta = &out;
    return io->run(IoPriority::Background, readMetaJob, &job);
}

bool Downloader::writeMeta(const RegionMeta& out) {
    RegionMeta copy = out;
    MetaJob job {};
    snprintf(job.path, sizeof(job.path), "%s/maps/%s/meta.json", rootPath, copy.name);
    job.meta = &copy;
    return io->run(IoPriority::Background, writeMetaJob, &job);
}

bool Downloader::tileOnCard(const char* region, const TileKey& tile, bool& inThisRegion) {
    OnCardJob job { rootPath, store, region, tile, false, false };
    const bool found = io->run(IoPriority::Background, onCardJob, &job);
    inThisRegion = job.inThisRegion;
    return found;
}

Downloader::OnCard Downloader::findTile(const char* region, const TileKey& tile) {
    OnCardJob job { rootPath, store, region, tile, false, false };
    if (io->run(IoPriority::Background, onCardJob, &job)) {
        return job.inThisRegion ? OnCard::InRegion : OnCard::Shared;
    }
    return job.inBrowsed ? OnCard::InBrowsed : OnCard::Missing;
}

BrowsedMove Downloader::moveBrowsedTile(Connection& connection, const TileKey& tile, uint64_t& size) {
    // Renames write the folder entries on the card, which shares its bus with the display: only while the map is idle
    while (!idle && !stopRequested && !cancelRequested) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    MoveJob job { rootPath, store, meta.name, tile, meta.version, BrowsedMove::Failed, 0 };
    if (!io->run(IoPriority::Background, moveJob, &job)) {
        return BrowsedMove::Failed;
    }
    size = job.size;
    return job.result;
}

void Downloader::removeBrowsedCopy(Connection& connection, const TileKey& tile) {
    RemoveBrowsedJob job { rootPath, tile, 0 };
    io->run(IoPriority::Background, removeBrowsedJob, &job);
    connection.browsedRemoved += job.size;
}

void Downloader::settleBrowsed(uint64_t removedBytes) {
    if (removedBytes == 0) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool inMemory = browseMetaReady;
    if (inMemory) {
        browseMeta.bytes -= std::min(browseMeta.bytes, removedBytes);
    }
    RegionMeta copy = browseMeta;
    xSemaphoreGive(mutex);
    if (!inMemory && !readMeta(BROWSE_REGION, copy)) {
        return;
    }
    if (!inMemory) {
        copy.bytes -= std::min(copy.bytes, removedBytes);
    }
    writeMeta(copy);
}

int Downloader::movedTiles() const {
    int total = 0;
    for (int i = 0; i < CONNECTIONS; i++) {
        total += connections[i].movedTiles;
    }
    return total;
}

bool Downloader::downloadTile(Connection& connection, const char* region, const TileKey& tile, int& status, bool waitForIdle) {
    char url[200];
    if (!formatTileUrl(urlTemplate, tile, url, sizeof(url))) {
        return false;
    }
    connection.body->clear();
    auto* client = static_cast<esp_http_client_handle_t>(connection.client);
    esp_http_client_set_url(client, url);
    const esp_err_t result = perform(connection);
    status = esp_http_client_get_status_code(client);
    const size_t size = connection.body->size();
    if (result != ESP_OK || connection.body->failed() || size > MAX_TILE_BYTES || (status != 200 && status != 204 && status != 404)) {
        if (connection.body->failed()) {
            logError("Tile %d/%d/%d: no memory for %u bytes", tile.z, tile.x, tile.y, static_cast<unsigned>(size));
        }
        connection.body->trim();
        return false;
    }
    if (status != 200 || size == 0) {
        // An empty tile, the sea or empty land at this zoom level
        return true;
    }
    // Card work for downloads waits until the map has been left alone for a moment, writes hold the display's shared
    // bus. A tile the screen needs now is written at once
    while (waitForIdle && !idle && !stopRequested && !cancelRequested) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    StoreTileJob job { rootPath, region, tile, connection.body, connection.lastFolder, false };
    bool stored = io->run(IoPriority::Background, storeTileJob, &job);
    for (int attempt = 0; !stored && job.yielded && attempt < 20 && !stopRequested; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(50));
        stored = io->run(IoPriority::Background, storeTileJob, &job);
    }
    connection.body->trim();
    if (!stored) {
        return false;
    }
    connection.received += static_cast<uint64_t>(size);
    return true;
}

bool Downloader::claimIndex(int64_t& index, int connectionIndex) {
    while (true) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        const bool available = nextIndex < meta.total && !failed && !cancelRequested && !stopRequested;
        if (!available) {
            xSemaphoreGive(mutex);
            return false;
        }
        // Tiles up to ALONE_MAX_ZOOM can be over 1 MB each: one is downloaded at a time, with no other tile next to it
        TileKey tile {};
        bool large = tileAtIndex(meta.plan(), nextIndex, tile) && tile.z <= ALONE_MAX_ZOOM;
        bool othersActive = false;
        for (int i = 0; i < CONNECTIONS; i++) {
            if (i == connectionIndex || activeIndex[i] < 0) {
                continue;
            }
            othersActive = true;
            large = large || (tileAtIndex(meta.plan(), activeIndex[i], tile) && tile.z <= ALONE_MAX_ZOOM);
        }
        if (!large || !othersActive) {
            index = nextIndex++;
            activeIndex[connectionIndex] = index;
            xSemaphoreGive(mutex);
            return true;
        }
        xSemaphoreGive(mutex);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void Downloader::finishIndex(int connectionIndex) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    activeIndex[connectionIndex] = -1;
    // Tiles finish out of order, the saved count is the first tile that is not finished yet
    int64_t done = nextIndex;
    for (int i = 0; i < CONNECTIONS; i++) {
        if (activeIndex[i] >= 0 && activeIndex[i] < done) {
            done = activeIndex[i];
        }
    }
    meta.done = done;
    uint64_t bytes = 0;
    for (int i = 0; i < CONNECTIONS; i++) {
        bytes += connections[i].received + connections[i].moved;
    }
    current.done = done;
    current.bytes = meta.bytes + bytes;
    sinceSave++;
    const bool save = sinceSave >= SAVE_EVERY_TILES;
    if (save) {
        sinceSave = 0;
    }
    xSemaphoreGive(mutex);
    if (save) {
        saveProgress();
        allTilesChanged = true;
        tilesChanged = true;
    }
}

void Downloader::saveProgress() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    RegionMeta copy = meta;
    uint64_t bytes = 0;
    uint64_t moved = 0;
    for (int i = 0; i < CONNECTIONS; i++) {
        bytes += connections[i].received + connections[i].moved;
        moved += connections[i].moved;
    }
    copy.bytes = meta.bytes + bytes;
    copy.moved = meta.moved + moved;
    xSemaphoreGive(mutex);
    writeMeta(copy);
}

void Downloader::connectionMain(void* context) {
    auto* connection = static_cast<Connection*>(context);
    connection->owner->connectionLoop(*connection);
    xSemaphoreGive(connection->owner->connectionsDone);
    vTaskSuspend(nullptr);
}

void Downloader::connectionLoop(Connection& connection) {
    int64_t index;
    while (claimIndex(index, connection.index)) {
        TileKey tile {};
        if (!tileForIndex(meta, index, tile)) {
            finishIndex(connection.index);
            continue;
        }
        const OnCard onCard = findTile(meta.name, tile);
        if (onCard == OnCard::InRegion || onCard == OnCard::Shared) {
            finishIndex(connection.index);
            continue;
        }
        // A tile browsed earlier moves into the region, unless it came from another version of the tiles
        bool staleBrowsed = false;
        if (onCard == OnCard::InBrowsed) {
            uint64_t size = 0;
            const BrowsedMove move = moveBrowsedTile(connection, tile, size);
            if (move == BrowsedMove::Moved) {
                xSemaphoreTake(mutex, portMAX_DELAY);
                connection.moved += size;
                connection.movedTiles++;
                xSemaphoreGive(mutex);
                finishIndex(connection.index);
                continue;
            }
            staleBrowsed = move == BrowsedMove::Stale;
        }
        bool ok = false;
        int status = 0;
        const uint32_t tileStart = nowMs();
        const uint64_t receivedBefore = connection.received;
        for (int attempt = 1; attempt <= MAX_ATTEMPTS && !ok && !cancelRequested; attempt++) {
            takeClient(connection);
            ok = connection.client != nullptr && downloadTile(connection, meta.name, tile, status);
            if (!ok) {
                logError("[%d] Tile %d/%d/%d failed (HTTP %d), attempt %d", connection.index, tile.z, tile.x, tile.y, status, attempt);
                dropClient(connection);
                vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            }
        }
        if (!ok) {
            failed = true;
        }
        if (ok && staleBrowsed) {
            // Never two versions of a tile: the old browsed copy goes once the new one is stored
            removeBrowsedCopy(connection, tile);
        }
        // The limit holds for the real size too, the estimate can be lower for dense city centres. Moved tiles were
        // not downloaded and do not count
        xSemaphoreTake(mutex, portMAX_DELAY);
        uint64_t bytes = meta.bytes - std::min(meta.bytes, meta.moved);
        for (int i = 0; i < CONNECTIONS; i++) {
            bytes += connections[i].received;
        }
        xSemaphoreGive(mutex);
        if (bytes > MAX_DOWNLOAD_BYTES) {
            failed = true;
        }
        if (ok) {
            logInfo("[%d] Tile %d/%d/%d: %u bytes in %u ms (HTTP %d)", connection.index, tile.z, tile.x, tile.y,
                static_cast<unsigned>(connection.received - receivedBefore), nowMs() - tileStart, status);
            finishIndex(connection.index);
        }
    }
    // Kept open for the next download, browsing or an update
    giveClient(connection);
}

void Downloader::runDownload() {
    if (!readMeta(current.region, meta)) {
        setStatus(State::Failed, "Region information is missing");
        return;
    }
    snprintf(meta.name, sizeof(meta.name), "%s", current.region);
    if (meta.total > MAX_DOWNLOAD_TILES) {
        setStatus(State::Failed, "Area is larger than 3000 tiles");
        return;
    }
    if (!loadOutlineTiles(meta)) {
        setStatus(State::Failed, outlineNoMemory ? "Not enough memory for the region list" : "Region outline is missing");
        return;
    }
    if (!wifiConnected()) {
        setStatus(State::Failed, "No Wi-Fi connection");
        return;
    }
    setStatus(State::Connecting, "Connecting to OpenFreeMap");
    logMemory("download start");
    connections[0].owner = this;
    connections[0].index = 0;
    connections[0].body = &bodies[0];
    const bool tileJson = ensureTileJson(connections[0]);
    giveClient(connections[0]);
    bodies[0].release();
    if (!tileJson) {
        setStatus(State::Failed, "OpenFreeMap is not reachable");
        return;
    }
    if (meta.source[0] == '\0') {
        snprintf(meta.source, sizeof(meta.source), "%s", urlTemplate);
    }
    snprintf(meta.version, sizeof(meta.version), "%s", version);
    xSemaphoreTake(mutex, portMAX_DELAY);
    current.total = meta.total;
    current.done = meta.done;
    current.bytes = meta.bytes;
    nextIndex = meta.done;
    sinceSave = 0;
    failed = false;
    xSemaphoreGive(mutex);
    setStatus(State::Downloading, "Downloading");

    const uint32_t start = nowMs();
    int started = 0;
    for (int i = 0; i < CONNECTIONS; i++) {
        Connection& connection = connections[i];
        connection = { this, i, nullptr, &bodies[i], 0, nullptr, "" };
        activeIndex[i] = -1;
        char name[16];
        snprintf(name, sizeof(name), "maps-http%d", i);
        if (startNetworkTask(connectionMain, name, CONNECTION_STACK, &connection, &connection.task)) {
            started++;
        } else {
            connection.task = nullptr;
        }
    }
    for (int i = 0; i < started; i++) {
        xSemaphoreTake(connectionsDone, portMAX_DELAY);
    }
    for (int i = 0; i < CONNECTIONS; i++) {
        if (connections[i].task != nullptr) {
            endNetworkTask(connections[i].task);
            connections[i].task = nullptr;
        }
        bodies[i].release();
    }
    freeOutlineTiles();
    logMemory("download end");

    xSemaphoreTake(mutex, portMAX_DELAY);
    uint64_t leftBrowsed = 0;
    int moveCount = 0;
    for (int i = 0; i < CONNECTIONS; i++) {
        meta.bytes += connections[i].received + connections[i].moved;
        meta.moved += connections[i].moved;
        leftBrowsed += connections[i].moved + connections[i].browsedRemoved;
        moveCount += connections[i].movedTiles;
        connections[i].received = 0;
        connections[i].moved = 0;
        connections[i].browsedRemoved = 0;
    }
    if (!failed && !cancelRequested) {
        meta.done = meta.total;
    }
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    strftime(meta.date, sizeof(meta.date), "%Y-%m-%d", &local);
    current.done = meta.done;
    current.bytes = meta.bytes;
    xSemaphoreGive(mutex);
    // An area framed on the map is named after the nearest town in its own tiles, now that they are on the card
    char place[sizeof(meta.title)];
    if (meta.isComplete() && meta.outline[0] == '\0' && findPlaceName(meta, place, sizeof(place))) {
        logInfo("Region %s is named %s", meta.name, place);
        setRegionTitle(meta, place);
    }
    writeMeta(meta);
    store->setRegionVersion(meta.name, meta.version);
    // The browsed region is smaller by the tiles that moved out of it and the stale copies replaced
    settleBrowsed(leftBrowsed);
    if (moveCount > 0) {
        logInfo("%d browsed tiles moved into %s", moveCount, meta.name);
    }
    allTilesChanged = true;
    tilesChanged = true;

    const uint32_t seconds = (nowMs() - start) / 1000;
    char message[64];
    if (meta.bytes > MAX_DOWNLOAD_BYTES) {
        setStatus(State::Failed, "Stopped at the 50 MB limit");
    } else if (cancelRequested) {
        // Before the failure check: a request cut off by the cancel is no error to retry
        setStatus(State::Done, "Download paused", true);
    } else if (failed) {
        setStatus(State::Failed, "Download failed, it continues later");
    } else {
        // The heights of a finished region come with it (Settings, Terrain); a failure there leaves the map as it is
        if (terrainWithDownloads && meta.isComplete()) {
            runTerrain(meta);
        }
        if (trailsWithDownloads && meta.isComplete() && !cancelRequested) {
            runTrails(meta);
        }
        if (!cancelRequested) {
            snprintf(message, sizeof(message), "Downloaded %d KB in %u s", static_cast<int>(meta.bytes / 1024), static_cast<unsigned>(seconds));
            setStatus(State::Done, message);
        }
    }
}

namespace {

/** A downloaded body as a byte stream, chunk by chunk. */
class ChunkedInput final : public ByteInput {
public:
    explicit ChunkedInput(const ChunkedBody& body) : body(body) {}
    int read(uint8_t* buffer, size_t size) override {
        size_t done = 0;
        while (done < size && position < body.size()) {
            const int chunk = static_cast<int>(position / ChunkedBody::CHUNK_BYTES);
            const size_t offset = position % ChunkedBody::CHUNK_BYTES;
            const size_t step = std::min(size - done, body.chunkSize(chunk) - offset);
            if (step == 0) {
                break;
            }
            memcpy(buffer + done, body.chunk(chunk) + offset, step);
            done += step;
            position += step;
        }
        return static_cast<int>(done);
    }
    bool rewind() override {
        position = 0;
        return true;
    }

private:
    const ChunkedBody& body;
    size_t position = 0;
};

struct TerrainFileJob {
    const char* root;
    const char* region;
    TileKey tile;
    const uint8_t* bytes;
    bool exists;
    /** The existence check looks in every region of it (the map reads heights from any). */
    const TileStore* store;
};

bool terrainExistsJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<TerrainFileJob*>(context);
    char path[PATH_MAX_LENGTH];
    job->exists = terrainPath(path, sizeof(path), job->root, job->region, job->tile) && fileExists(path);
    // Another region with the same heights: not fetched twice (the map would only be drawn again)
    // Only regions whose area holds the tile: each look on the card costs tens of milliseconds (24 regions took 1 s)
    for (int i = 0; !job->exists && job->store != nullptr && i < job->store->regionCount(); i++) {
        if (job->store->mayHave(i, job->tile)) {
            job->exists = terrainPath(path, sizeof(path), job->root, job->store->regionName(i), job->tile) && fileExists(path);
        }
    }
    return true;
}

bool storeTerrainJob(IoTask& io, void* context) {
    auto* job = static_cast<TerrainFileJob*>(context);
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH];
    if (!terrainPath(path, sizeof(path), job->root, job->region, job->tile) ||
        !terrainTempPath(temporary, sizeof(temporary), job->root, job->region, job->tile)) {
        return false;
    }
    makeFolders(job->root, job->region, job->tile);
    return io.writeFile(path, temporary, job->bytes, TERRAIN_FILE_BYTES, true);
}

/** maps/<region>/terrain.ok: all heights of the region are on the card. */
struct MarkerJob {
    const char* root;
    const char* region;
    bool exists;
};

bool writeTerrainMarkerJob(IoTask& io, void* context) {
    auto* job = static_cast<MarkerJob*>(context);
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/maps/%s/terrain.ok", job->root, job->region);
    snprintf(temporary, sizeof(temporary), "%s/maps/%s/terrain.tmp", job->root, job->region);
    static const uint8_t TEXT[] = { 'o', 'k', '\n' };
    return io.writeFile(path, temporary, TEXT, sizeof(TEXT), true);
}

bool terrainMarkerJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<MarkerJob*>(context);
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/maps/%s/terrain.ok", job->root, job->region);
    job->exists = fileExists(path);
    return true;
}

struct TrailFileJob {
    const char* root;
    const char* region;
    TileKey tile;
    const GrowArray<uint8_t>* bytes;
};

bool storeTrailJob(IoTask& io, void* context) {
    auto* job = static_cast<TrailFileJob*>(context);
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH];
    if (!trailPath(path, sizeof(path), job->root, job->region, job->tile) ||
        !trailTempPath(temporary, sizeof(temporary), job->root, job->region, job->tile)) {
        return false;
    }
    makeFolders(job->root, job->region, job->tile);
    return io.writeFile(path, temporary, job->bytes->begin(), job->bytes->size(), true);
}

/**
 * The main public instance. It speaks TLS 1.3 only and has no plain HTTP (checked 15 Nov 2026), while Tactility's
 * mbedTLS build leaves TLS 1.3 out (CONFIG_MBEDTLS_SSL_PROTO_TLS1_3 is not set): until the firmware has it, the
 * handshake fails (docs/upstream/mbedtls-tls13.md). The TLS 1.2 instances (kumi.systems, private.coffee) did not
 * answer a region's query within 150 s.
 */
constexpr const char* OVERPASS_URL = "https://overpass-api.de/api/interpreter";
/** The server takes 20-40 s before it answers a region's query. */
constexpr int OVERPASS_TIMEOUT_MS = 120000;
constexpr uint32_t OVERPASS_BUSY_WAIT_MS = 30000;

constexpr const char* TERRAIN_URL = "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/%d/%d/%d.png";
/** An elevation tile is 70-140 KB; anything larger is not one. */
constexpr size_t MAX_TERRAIN_BYTES = 512 * 1024;
/** Terrain files come in pieces of this size (about 1.3 s each from the T-Deck), see runTerrainTiles. */
constexpr size_t TERRAIN_PIECE_BYTES = 32 * 1024;
/** Terrain starts this long after the last browsed tile. */
constexpr uint32_t TERRAIN_AFTER_BROWSE_MS = 2000;

} // namespace

bool Downloader::runTrails(const RegionMeta& region) {
    if (!wifiConnected()) {
        setStatus(State::Failed, "No Wi-Fi connection");
        return false;
    }
    char* post = static_cast<char*>(largeAlloc(1024));
    if (post == nullptr || !formatTrailQuery(region.box, post, 1024)) {
        largeFree(post);
        setStatus(State::Failed, "Trails could not be asked for");
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    snprintf(current.region, sizeof(current.region), "%s", region.name);
    current.total = 2;
    current.done = 0;
    current.bytes = 0;
    current.trails = true;
    xSemaphoreGive(mutex);
    setStatus(State::Connecting, "Asking for trails (up to a minute)");
    logMemory("trails start");
    const uint32_t start = nowMs();
    Connection& connection = connections[0];
    connection = { this, 0, nullptr, &bodies[0], 0, nullptr, "" };
    esp_http_client_config_t config = {};
    config.url = OVERPASS_URL;
    config.user_agent = USER_AGENT;
    config.timeout_ms = OVERPASS_TIMEOUT_MS;
    config.method = HTTP_METHOD_POST;
    config.cert_pem = ROOT_CERTIFICATES;
    config.cert_len = sizeof(ROOT_CERTIFICATES);
    config.event_handler = onHttpEvent;
    config.user_data = &connection;
    config.buffer_size = 4096;
    connection.client = esp_http_client_init(&config);
    bool fetched = false;
    bool noTls = false;
    int status = 0;
    if (connection.client != nullptr) {
        auto* client = static_cast<esp_http_client_handle_t>(connection.client);
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_header(client, "Accept-Encoding", "gzip");
        esp_http_client_set_post_field(client, post, static_cast<int>(strlen(post)));
        // One query at a time; when the server is busy (429, 504) it is asked again after 30 s, twice at most
        for (int attempt = 1; attempt <= 3 && !fetched && !cancelRequested && !stopRequested; attempt++) {
            connection.body->clear();
            const esp_err_t result = perform(connection);
            status = esp_http_client_get_status_code(client);
            fetched = result == ESP_OK && status == 200 && !connection.body->failed();
            if (!fetched && result == ESP_ERR_HTTP_CONNECT && !connection.connected) {
                // No secure connection at all (the firmware's TLS): trying again would only fail again
                noTls = true;
                logError("Trails: no TLS connection to %s (it needs TLS 1.3)", OVERPASS_URL);
                break;
            }
            if (!fetched) {
                logError("Trails for %s: HTTP %d (%s), attempt %d", region.name, status, esp_err_to_name(result), attempt);
                const uint32_t waitMs = status == 429 || status == 504 ? OVERPASS_BUSY_WAIT_MS : 5000;
                for (uint32_t waited = 0; attempt < 3 && waited < waitMs && !cancelRequested && !stopRequested; waited += 100) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
            }
        }
    }
    largeFree(post);
    dropClient(connection);
    const size_t received = connection.body->size();
    logInfo("Trails for %s: %u bytes in %u s (HTTP %d)", region.name, static_cast<unsigned>(received), static_cast<unsigned>((nowMs() - start) / 1000), status);
    // The answer, gzip or plain JSON, read in pieces into the parser; the lines collected per z12 tile
    void* tilerPlace = largeAlloc(sizeof(TrailTiler));
    void* parserPlace = largeAlloc(sizeof(TrailParser));
    void* inflaterPlace = largeAlloc(sizeof(InflateReader));
    auto* piece = static_cast<uint8_t*>(largeAlloc(4096));
    TrailTiler* tiler = tilerPlace != nullptr ? new (tilerPlace) TrailTiler() : nullptr;
    TrailParser* parser = parserPlace != nullptr ? new (parserPlace) TrailParser() : nullptr;
    InflateReader* inflater = inflaterPlace != nullptr ? new (inflaterPlace) InflateReader() : nullptr;
    bool ok = fetched && tiler != nullptr && parser != nullptr && inflater != nullptr && piece != nullptr && tiler->begin(region.box) &&
        parser->begin(tiler);
    int stored = 0;
    if (ok) {
        setStatus(State::Downloading, "Making the trail files");
        xSemaphoreTake(mutex, portMAX_DELAY);
        current.done = 1;
        current.bytes = received;
        xSemaphoreGive(mutex);
        ChunkedInput input(*connection.body);
        const bool gzip = received >= 2 && connection.body->chunk(0)[0] == 0x1F && connection.body->chunk(0)[1] == 0x8B;
        if (gzip) {
            ok = inflater->init() && inflater->begin(&input);
            while (ok) {
                const size_t got = inflater->read(piece, 4096);
                if (got == 0) {
                    break;
                }
                ok = parser->feed(piece, got);
            }
        } else {
            while (ok) {
                const int got = input.read(piece, 4096);
                if (got <= 0) {
                    break;
                }
                ok = parser->feed(piece, static_cast<size_t>(got));
            }
        }
        ok = ok && parser->finish() && !tiler->failed();
        logInfo("Trails for %s: %d routes, %d lines, %d points, %d tiles, %u KB held", region.name, parser->routeCount(), parser->lineCount(),
            parser->pointCount(), tiler->tileCount(), static_cast<unsigned>(tiler->memoryBytes() / 1024));
        connection.body->trim();
        GrowArray<uint8_t> file;
        for (int i = 0; ok && i < tiler->tileCount() && !cancelRequested && !stopRequested; i++) {
            file.clear();
            if (!tiler->buildTile(i, file)) {
                continue;
            }
            while (!idle && !stopRequested && !cancelRequested) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            TrailFileJob job { rootPath, region.name, tiler->tileAt(i), &file };
            if (io->run(IoPriority::Background, storeTrailJob, &job)) {
                stored++;
                noteTerrainFile(job.tile);
            } else {
                ok = false;
            }
        }
        file.release();
    }
    if (inflater != nullptr) {
        inflater->~InflateReader();
    }
    if (parser != nullptr) {
        parser->~TrailParser();
    }
    if (tiler != nullptr) {
        tiler->end();
        tiler->~TrailTiler();
    }
    largeFree(inflaterPlace);
    largeFree(parserPlace);
    largeFree(tilerPlace);
    largeFree(piece);
    bodies[0].release();
    logMemory("trails end");
    xSemaphoreTake(mutex, portMAX_DELAY);
    current.trails = false;
    current.done = 2;
    xSemaphoreGive(mutex);
    if (cancelRequested) {
        setStatus(State::Done, "Trails paused", true);
        return false;
    }
    if (!ok) {
        setStatus(State::Failed, noTls ? "Trails need TLS 1.3 in the firmware"
                                       : (status == 429 || status == 504 ? "The trail server is busy, try later" : "Trails could not be downloaded"));
        return false;
    }
    char message[64];
    snprintf(message, sizeof(message), "Trails: %d tiles in %u s", stored, static_cast<unsigned>((nowMs() - start) / 1000));
    setStatus(State::Done, message);
    return true;
}

bool Downloader::runTerrain(const RegionMeta& region, bool quiet) {
    const TileRange range = tileRange(region.box, TERRAIN_ZOOM);
    const int64_t count = range.count();
    if (count <= 0 || count > MAX_TERRAIN_TILES) {
        if (!quiet) {
            setStatus(State::Failed, "Area too large for terrain (300 tiles)");
        }
        return false;
    }
    auto* tiles = static_cast<TileKey*>(largeAlloc(static_cast<size_t>(count) * sizeof(TileKey)));
    if (tiles == nullptr) {
        if (!quiet) {
            setStatus(State::Failed, "Not enough memory for the terrain");
        }
        return false;
    }
    int listed = 0;
    for (int y = range.minY; y <= range.maxY; y++) {
        for (int x = range.minX; x <= range.maxX; x++) {
            tiles[listed++] = { TERRAIN_ZOOM, x, y };
        }
    }
    const bool ok = runTerrainTiles(region.name, tiles, listed, quiet);
    largeFree(tiles);
    if (ok) {
        // Every height of the region is on the card: the sweep after "Terrain on" skips it from now on
        MarkerJob marker { rootPath, region.name, false };
        io->run(IoPriority::Background, writeTerrainMarkerJob, &marker);
    }
    return ok;
}

bool Downloader::storeTerrain(void* job) {
    // A write gives way to the map's card work (a yielding job): tried again once the map is left alone
    for (int attempt = 0; attempt < 20 && !stopRequested && !cancelRequested; attempt++) {
        while (!idle && !stopRequested && !cancelRequested) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (io->run(IoPriority::Background, storeTerrainJob, job)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

bool Downloader::runTerrainTiles(const char* regionName, TileKey* tiles, int listed, bool quiet) {
    const bool idleOnly = quiet && strcmp(regionName, BROWSE_REGION) != 0;
    // The tiles already on the card first, without the network: nothing missing, nothing to connect for
    int count = 0;
    for (int i = 0; i < listed && !stopRequested; i++) {
        TerrainFileJob job { rootPath, regionName, tiles[i], nullptr, false, store };
        io->run(IoPriority::Background, terrainExistsJob, &job);
        if (!job.exists) {
            tiles[count++] = tiles[i];
        }
    }
    if (count == 0 || stopRequested) {
        return !stopRequested;
    }
    if (!wifiConnected()) {
        if (!quiet) {
            setStatus(State::Failed, "No Wi-Fi connection");
        }
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    snprintf(current.region, sizeof(current.region), "%s", regionName);
    current.total = count;
    current.done = 0;
    current.bytes = 0;
    current.terrain = true;
    xSemaphoreGive(mutex);
    if (!quiet) {
        setStatus(State::Connecting, "Connecting for terrain");
    }
    logMemory("terrain start");
    // Its own client: the pooled ones belong to OpenFreeMap and trust other roots
    Connection& connection = connections[0];
    connection = { this, 0, nullptr, &bodies[0], 0, nullptr, "" };
    esp_http_client_config_t config = {};
    config.url = "https://s3.amazonaws.com/elevation-tiles-prod/";
    config.user_agent = USER_AGENT;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.keep_alive_enable = true;
    config.cert_pem = TERRAIN_ROOT_CERTIFICATES;
    config.cert_len = sizeof(TERRAIN_ROOT_CERTIFICATES);
    config.event_handler = onHttpEvent;
    config.user_data = &connection;
    config.buffer_size = 4096;
    connection.client = esp_http_client_init(&config);
    auto* file = static_cast<uint8_t*>(largeAlloc(TERRAIN_FILE_BYTES));
    auto* rows = static_cast<uint8_t*>(largeAlloc(2048));
    void* inflaterPlace = largeAlloc(sizeof(InflateReader));
    InflateReader* inflater = inflaterPlace != nullptr ? new (inflaterPlace) InflateReader() : nullptr;
    bool ok = connection.client != nullptr && file != nullptr && rows != nullptr && inflater != nullptr && inflater->init();
    if (!ok) {
        setStatus(State::Failed, "Not enough memory for the terrain");
    }
    int stored = 0;
    uint64_t received = 0;
    const uint32_t start = nowMs();
    bool started = false;
    terrainYielded = false;
    for (int i = 0; ok && i < count; i++) {
        // Idle-time runs fetch while the map is left alone (drawing needs the internal RAM)
        while (quiet && (!idle || !viewComplete) && !cancelRequested && !stopRequested && !browsing && browseCount == 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        // An idle-time sweep stops at the first input, and every idle-time run once map tiles are browsed (two TLS
        // transfers ran out of internal RAM): they continue later
        if (cancelRequested || stopRequested || (idleOnly && !updatesAllowed)) {
            ok = false;
            break;
        }
        if (quiet && (browsing || browseCount > 0)) {
            terrainYielded = true;
            ok = false;
            break;
        }
        const TileKey tile = tiles[i];
        TerrainFileJob job { rootPath, regionName, tile, file, false, nullptr };
        char url[120];
        snprintf(url, sizeof(url), TERRAIN_URL, tile.z, tile.x, tile.y);
        bool fetched = false;
        int status = 0;
        // Idle-time runs: a request that failed while the map was moved (drawing took the internal RAM its TLS
        // needed) does not count, it is made again once the map is left alone; at most this many such repeats
        int interrupted = 0;
        for (int attempt = 1; attempt <= MAX_ATTEMPTS && !fetched && !cancelRequested && !stopRequested; attempt++) {
            while (quiet && (!idle || !viewComplete) && !cancelRequested && !stopRequested && !browsing && browseCount == 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (quiet && (browsing || browseCount > 0)) {
                terrainYielded = true;
                break;
            }
            // In pieces (HTTP ranges): at 25 KB/s a whole file took 4-5 s, and tiles for the screen waited for it (one
            // transfer at a time). Between pieces a quiet run gives way to browsing, so they wait at most one piece.
            auto* client = static_cast<esp_http_client_handle_t>(connection.client);
            connection.body->clear();
            esp_http_client_set_url(client, url);
            esp_err_t result = ESP_FAIL;
            bool pieceYielded = false;
            fetched = false;
            for (size_t from = 0;; from += TERRAIN_PIECE_BYTES) {
                char range[48];
                snprintf(range, sizeof(range), "bytes=%u-%u", static_cast<unsigned>(from), static_cast<unsigned>(from + TERRAIN_PIECE_BYTES - 1));
                esp_http_client_set_header(client, "Range", range);
                const size_t before = connection.body->size();
                result = perform(connection);
                status = esp_http_client_get_status_code(client);
                const size_t got = connection.body->size() - before;
                if (result != ESP_OK || connection.body->failed() || connection.body->size() > MAX_TERRAIN_BYTES) {
                    break;
                }
                if (status == 200 || status == 404) {
                    // The whole file at once (ranges not used by the server), or none
                    fetched = true;
                    break;
                }
                if (status == 416 && from > 0) {
                    // Past the end: the file's size was a whole number of pieces
                    fetched = true;
                    status = 200;
                    break;
                }
                if (status != 206) {
                    break;
                }
                if (got < TERRAIN_PIECE_BYTES) {
                    fetched = true;
                    status = 200;
                    break;
                }
                if (quiet && (browsing || browseCount > 0)) {
                    pieceYielded = true;
                    break;
                }
            }
            esp_http_client_delete_header(client, "Range");
            if (pieceYielded) {
                terrainYielded = true;
                break;
            }
            if (!fetched) {
                logError("Heights %d/%d/%d failed (HTTP %d), attempt %d", tile.z, tile.x, tile.y, status, attempt);
                logMemory("heights failed");
                if (quiet && (!idle || !viewComplete) && interrupted < 5) {
                    interrupted++;
                    attempt--;
                    continue;
                }
                vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
            }
        }
        if (!fetched) {
            ok = false;
            break;
        }
        if (!started) {
            started = true;
            setStatus(State::Downloading, "Downloading terrain");
        }
        if (status == 200) {
            received += connection.body->size();
            ChunkedInput input(*connection.body);
            const bool decoded = terrariumToHeightFile(input, *inflater, rows, file);
            // Card work waits until the map is left alone, as for map tiles
            while (decoded && !idle && !stopRequested && !cancelRequested) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (!decoded) {
                logError("Heights %d/%d/%d: not a Terrarium PNG (%u bytes)", tile.z, tile.x, tile.y, static_cast<unsigned>(connection.body->size()));
            } else if (storeTerrain(&job)) {
                stored++;
                noteTerrainFile(job.tile);
            } else {
                logError("Heights %d/%d/%d could not be stored", tile.z, tile.x, tile.y);
                ok = false;
            }
        }
        connection.body->trim();
        xSemaphoreTake(mutex, portMAX_DELAY);
        current.done++;
        current.bytes = received;
        xSemaphoreGive(mutex);
    }
    if (inflater != nullptr) {
        inflater->~InflateReader();
        largeFree(inflater);
    }
    largeFree(rows);
    largeFree(file);
    dropClient(connection);
    bodies[0].release();
    logInfo("Heights for %s: %d of %d missing tiles stored, %u KB in %u s", regionName, stored, count,
        static_cast<unsigned>(received / 1024), static_cast<unsigned>((nowMs() - start) / 1000));
    logMemory("terrain end");
    xSemaphoreTake(mutex, portMAX_DELAY);
    current.terrain = false;
    xSemaphoreGive(mutex);
    // An idle-time run that showed its progress always ends it, or the progress pill stays
    if (cancelRequested || terrainYielded || (idleOnly && !updatesAllowed)) {
        if (!quiet) {
            setStatus(State::Done, "Terrain paused", true);
        } else if (started) {
            setStatus(State::Idle, "");
        }
        return false;
    }
    if (!ok) {
        if (!quiet) {
            setStatus(State::Failed, "Terrain could not be downloaded");
        } else if (started) {
            setStatus(State::Failed, "Terrain failed, tried again later");
        }
        return false;
    }
    if (!quiet || stored > 0) {
        char message[64];
        snprintf(message, sizeof(message), "Terrain: %d tile%s, %u KB", stored, stored == 1 ? "" : "s", static_cast<unsigned>(received / 1024));
        setStatus(State::Done, message);
    }
    return true;
}

struct DeleteColumnJob {
    const char* root;
    const char* region;
    int z;
    int x;
    const RegionMeta* others;
    int otherCount;
};

namespace {

bool deleteColumnJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<DeleteColumnJob*>(context);
    char column[PATH_MAX_LENGTH];
    snprintf(column, sizeof(column), "%s/maps/%s/%d/%d", job->root, job->region, job->z, job->x);
    struct Files {
        DeleteColumnJob* job;
        const char* column;
    } files { job, column };
    listDirectory(column, [](const char* fileName, bool fileIsDirectory, void* fileData) {
        auto* f = static_cast<Files*>(fileData);
        if (fileIsDirectory) {
            return true;
        }
        char file[PATH_MAX_LENGTH];
        snprintf(file, sizeof(file), "%s/%s", f->column, fileName);
        // Only originals move to a region that still needs them, prepared copies are made again there
        const size_t nameLength = strlen(fileName);
        if (nameLength < 5 || (strcmp(fileName + nameLength - 4, ".pbf") != 0 && strcmp(fileName + nameLength - 4, ".PBF") != 0)) {
            remove(file);
            return true;
        }
        const TileKey tile { f->job->z, f->job->x, atoi(fileName) };
        for (int i = 0; i < f->job->otherCount; i++) {
            const RegionMeta& other = f->job->others[i];
            const TileRange range = tileRange(other.box, tile.z);
            char otherPath[PATH_MAX_LENGTH];
            if (tile.z >= other.minZoom && tile.z <= other.maxZoom && tile.x >= range.minX && tile.x <= range.maxX &&
                tile.y >= range.minY && tile.y <= range.maxY && tilePath(otherPath, sizeof(otherPath), f->job->root, other.name, tile) &&
                !fileExists(otherPath)) {
                makeFolders(f->job->root, other.name, tile);
                if (rename(file, otherPath) == 0) {
                    return true;
                }
            }
        }
        remove(file);
        return true;
    }, &files);
    rmdir(column);
    return true;
}

} // namespace

bool Downloader::ensureTileJson(Connection& connection) {
    // The region download and the browse task may both need it first
    xSemaphoreTake(tileJsonLock, portMAX_DELAY);
    const bool ok = urlTemplate[0] != '\0' || fetchTileJson(connection);
    xSemaphoreGive(tileJsonLock);
    return ok;
}

bool Downloader::isOnline() {
    return wifiConnected();
}

void Downloader::keepBrowse(int sourceZoom, int minX, int minY, int maxX, int maxY) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    int kept = 0;
    for (int i = 0; i < browseCount; i++) {
        const TileKey& tile = browseQueue[i];
        const bool column = minX <= maxX ? tile.x >= minX && tile.x <= maxX : tile.x >= minX || tile.x <= maxX;
        if (tile.z == sourceZoom && column && tile.y >= minY && tile.y <= maxY) {
            browseQueue[kept++] = tile;
        }
    }
    // Dropped tiles no longer count towards "Fetching detail done/total"
    browseTotal -= browseCount - kept;
    browseCount = kept;
    xSemaphoreGive(mutex);
}

void Downloader::browse(const TileKey* tiles, int count) {
    if (browseTask == nullptr) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool added = false;
    if (browseCount == 0 && !browsing) {
        // A new batch: its progress counts from zero
        browseDone = 0;
        browseTotal = 0;
    }
    for (int i = 0; i < count; i++) {
        bool known = false;
        for (int j = 0; j < browseCount && !known; j++) {
            known = browseQueue[j] == tiles[i];
        }
        for (int j = 0; j < browseEmptyCount && !known; j++) {
            known = browseEmpty[j] == tiles[i];
        }
        if (known || !isValidTile(tiles[i])) {
            continue;
        }
        if (browseCount == BROWSE_QUEUE) {
            // The oldest request goes, the newest view matters most
            for (int j = 1; j < BROWSE_QUEUE; j++) {
                browseQueue[j - 1] = browseQueue[j];
            }
            browseCount--;
            browseTotal--;
        }
        browseQueue[browseCount++] = tiles[i];
        browseTotal++;
        added = true;
    }
    xSemaphoreGive(mutex);
    if (added) {
        xSemaphoreGive(browseWake);
    }
}

void Downloader::prepareBrowsing() {
    if (browseTask != nullptr) {
        warmUp = true;
        xSemaphoreGive(browseWake);
    }
}

void Downloader::browseMain(void* context) {
    auto* downloader = static_cast<Downloader*>(context);
    downloader->browseLoop();
    xSemaphoreGive(downloader->browseStopped);
    vTaskSuspend(nullptr);
}

bool Downloader::ensureBrowseRegion(const TileKey& tile) {
    // The region's box grows with the tiles
    const double west = worldXToLon(static_cast<double>(tile.x) * TILE_SIZE, tile.z);
    const double east = worldXToLon(static_cast<double>(tile.x + 1) * TILE_SIZE, tile.z);
    const double north = worldYToLat(static_cast<double>(tile.y) * TILE_SIZE, tile.z);
    const double south = worldYToLat(static_cast<double>(tile.y + 1) * TILE_SIZE, tile.z);
    if (!browseMetaReady) {
        if (!readMeta(BROWSE_REGION, browseMeta)) {
            browseMeta = {};
            browseMeta.box = { west, south, east, north };
            auto makeFolder = [&] {
                char folder[PATH_MAX_LENGTH];
                snprintf(folder, sizeof(folder), "%s/maps/%s", rootPath, BROWSE_REGION);
                mkdir(folder, 0777);
                return true;
            };
            runOnCard(*io, makeFolder, IoPriority::Background);
        }
        snprintf(browseMeta.name, sizeof(browseMeta.name), "%s", BROWSE_REGION);
        browseMeta.minZoom = 0;
        browseMeta.maxZoom = MAX_SOURCE_ZOOM;
        browseMeta.total = 1;
        browseMeta.done = 1;
        snprintf(browseMeta.version, sizeof(browseMeta.version), "%s", version);
        time_t now = time(nullptr);
        struct tm local {};
        localtime_r(&now, &local);
        strftime(browseMeta.date, sizeof(browseMeta.date), "%Y-%m-%d", &local);
        if (!writeMeta(browseMeta)) {
            return false;
        }
        store->addRegion(BROWSE_REGION);
        store->setRegionVersion(BROWSE_REGION, version);
        browseMetaReady = true;
    }
    browseMeta.box.west = std::min(browseMeta.box.west, west);
    browseMeta.box.east = std::max(browseMeta.box.east, east);
    browseMeta.box.south = std::min(browseMeta.box.south, south);
    browseMeta.box.north = std::max(browseMeta.box.north, north);
    store->setRegionBounds(BROWSE_REGION, browseMeta.box, browseMeta.minZoom, browseMeta.maxZoom);
    return true;
}

bool Downloader::takeTilesChanged(TileKey* tiles, int& count) {
    if (!tilesChanged.exchange(false)) {
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    count = allTilesChanged.exchange(false) ? -1 : arrivedCount;
    for (int i = 0; i < count; i++) {
        tiles[i] = arrived[i];
    }
    arrivedCount = 0;
    xSemaphoreGive(mutex);
    return true;
}

bool Downloader::fetchBrowseTile(const TileKey& tile) {
    bool inThisRegion = false;
    if (tileOnCard(BROWSE_REGION, tile, inThisRegion)) {
        return true;
    }
    if (!ensureBrowseRegion(tile)) {
        return false;
    }
    Connection& connection = browseConnection;
    const uint64_t before = connection.received;
    const uint32_t startMs = nowMs();
    int status = 0;
    bool ok = false;
    for (int attempt = 1; attempt <= 2 && !ok && !stopRequested; attempt++) {
        takeClient(connection);
        ok = connection.client != nullptr && downloadTile(connection, BROWSE_REGION, tile, status, false);
        if (!ok) {
            dropClient(connection);
        }
    }
    if (!ok) {
        logError("Browse: tile %d/%d/%d failed (HTTP %d)", tile.z, tile.x, tile.y, status);
        return false;
    }
    if (connection.received == before) {
        // No data there: remembered, so it is not asked for again
        xSemaphoreTake(mutex, portMAX_DELAY);
        browseEmpty[browseEmptyNext] = tile;
        browseEmptyNext = (browseEmptyNext + 1) % BROWSE_EMPTY;
        if (browseEmptyCount < BROWSE_EMPTY) {
            browseEmptyCount++;
        }
        xSemaphoreGive(mutex);
        return true;
    }
    browseMeta.bytes += connection.received - before;
    logInfo("Browse: tile %d/%d/%d, %u bytes in %u ms", tile.z, tile.x, tile.y, static_cast<unsigned>(connection.received - before), nowMs() - startMs);
    if (++browseUnsaved >= 5) {
        browseUnsaved = 0;
        writeMeta(browseMeta);
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (arrivedCount < MAX_ARRIVED) {
        arrived[arrivedCount++] = tile;
    } else {
        allTilesChanged = true;
    }
    xSemaphoreGive(mutex);
    tilesChanged = true;
    return true;
}

void Downloader::browseLoop() {
    browseConnection = { this, CONNECTIONS, nullptr, &browseBody, 0, nullptr, "" };
    while (!stopRequested) {
        if (xSemaphoreTake(browseWake, SWEEP_TICKS) != pdTRUE) {
            closeIdleClients(IDLE_CLOSE_MS);
            continue;
        }
        if (warmUp.exchange(false) && wifiConnected()) {
            ensureTileJson(browseConnection);
        }
        while (!stopRequested) {
            TileKey tile {};
            xSemaphoreTake(mutex, portMAX_DELAY);
            const bool any = browseCount > 0;
            if (any) {
                tile = browseQueue[0];
                for (int i = 1; i < browseCount; i++) {
                    browseQueue[i - 1] = browseQueue[i];
                }
                browseCount--;
            }
            xSemaphoreGive(mutex);
            if (!any) {
                break;
            }
            if (!wifiConnected() || !ensureTileJson(browseConnection)) {
                // Offline: the map says so, the requests are dropped and come again with the next view
                xSemaphoreTake(mutex, portMAX_DELAY);
                browseCount = 0;
                browseDone = 0;
                browseTotal = 0;
                xSemaphoreGive(mutex);
                break;
            }
            browsing = true;
            const bool fetched = fetchBrowseTile(tile);
            xSemaphoreTake(mutex, portMAX_DELAY);
            browseDone++;
            xSemaphoreGive(mutex);
            if (!fetched) {
                // A failing server is not asked again at once
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        browsing = false;
        browseEndedAt = nowMs();
        // Terrain for the tiles just fetched starts soon (TERRAIN_AFTER_BROWSE_MS), looked at from the next poll
        if (terrainWithDownloads) {
            xSemaphoreGive(wake);
        }
        giveClient(browseConnection);
        browseBody.release();
        if (browseUnsaved > 0) {
            browseUnsaved = 0;
            writeMeta(browseMeta);
        }
    }
    dropClient(browseConnection);
}

void Downloader::noteTerrainFile(const TileKey& tile) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool known = false;
    for (int i = 0; i < terrainArrivedCount && !known; i++) {
        known = terrainArrived[i] == tile;
    }
    if (!known && terrainArrivedCount < MAX_TERRAIN_ARRIVED) {
        terrainArrived[terrainArrivedCount++] = tile;
    } else if (!known) {
        terrainArrivedAll = true;
    }
    xSemaphoreGive(mutex);
    terrainChanged = true;
}

bool Downloader::takeTerrainChanged(TileKey* files, int& count) {
    if (!terrainChanged.exchange(false)) {
        return false;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    count = terrainArrivedAll ? -1 : terrainArrivedCount;
    for (int i = 0; i < terrainArrivedCount; i++) {
        files[i] = terrainArrived[i];
    }
    terrainArrivedCount = 0;
    terrainArrivedAll = false;
    xSemaphoreGive(mutex);
    return true;
}

void Downloader::setTerrainWithDownloads(bool value) {
    if (value && !terrainWithDownloads) {
        // Switched on (or on at start): every downloaded region gets its heights, in idle time
        terrainSweep = true;
    }
    terrainWithDownloads = value;
}

void Downloader::queueTerrain(const TileKey& tile) {
    if (!terrainWithDownloads || tile.z != TERRAIN_ZOOM) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool known = false;
    for (int i = 0; i < browseTerrainCount && !known; i++) {
        known = browseTerrain[i] == tile;
    }
    for (int i = 0; i < BROWSE_TERRAIN && !known; i++) {
        known = browseTerrainSeen[i] == tile;
    }
    if (!known && browseTerrainCount < BROWSE_TERRAIN) {
        browseTerrain[browseTerrainCount++] = tile;
        browseTerrainSeen[browseTerrainSeenNext] = tile;
        browseTerrainSeenNext = (browseTerrainSeenNext + 1) % BROWSE_TERRAIN;
    }
    const bool added = browseTerrainCount > 0;
    xSemaphoreGive(mutex);
    if (added && viewComplete && !browsing) {
        // Nothing to wait for: the control task looks at it now
        xSemaphoreGive(wake);
    }
}

bool Downloader::claimIdle() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool jobless = job == Job::None && !busy;
    if (jobless) {
        busy = true;
    }
    xSemaphoreGive(mutex);
    if (jobless) {
        cancelRequested = false;
    }
    return jobless;
}

void Downloader::releaseIdle() {
    xSemaphoreTake(mutex, portMAX_DELAY);
    busy = job != Job::None;
    xSemaphoreGive(mutex);
    if (busy) {
        xSemaphoreGive(wake);
    }
}

void Downloader::runPendingTerrain() {
    // Not while tiles are browsed, and after a failure not for two minutes: a second TLS connection next to the
    // browse one found no internal RAM for its AES buffers on the T-Deck (and the heap was damaged after it)
    if (!wifiConnected() || browsing || browseCount > 0 || (terrainFailedMs != 0 && nowMs() - terrainFailedMs < TERRAIN_RETRY_MS)) {
        return;
    }
    // A moment after the last browsed tile: while the map is panned, terrain and browsing otherwise took turns, and each
    // browsed tile after a terrain run paid a new handshake (its connection is closed for the terrain's TLS memory)
    if (browseEndedAt != 0 && nowMs() - browseEndedAt < TERRAIN_AFTER_BROWSE_MS) {
        return;
    }
    // Only while the map is left alone with internal RAM to spare: drawing a view took it down to 15 KB (largest
    // block 7 KB) and the transfer next to it broke off for its AES buffers
    if (!idle || !viewComplete) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool browsedWaiting = browseTerrainCount > 0;
    xSemaphoreGive(mutex);
    if (!browsedWaiting && !(terrainSweep && updatesAllowed)) {
        return;
    }
    // The kept OpenFreeMap connections give their TLS memory back first: kept open, the two sessions took internal RAM
    // down to 6 KB during a terrain transfer and its AES buffers failed (16 Nov). The next browsed tile pays a handshake.
    closeIdleClients(0);
    if (heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < TERRAIN_INTERNAL_BLOCK) {
        return;
    }
    // Heights of browsed areas: a few z12 tiles, fetched whenever they are wanted
    TileKey tiles[BROWSE_TERRAIN];
    xSemaphoreTake(mutex, portMAX_DELAY);
    const int count = browseTerrainCount;
    for (int i = 0; i < count; i++) {
        tiles[i] = browseTerrain[i];
    }
    xSemaphoreGive(mutex);
    if (count > 0 && claimIdle()) {
        xSemaphoreTake(mutex, portMAX_DELAY);
        browseTerrainCount = 0;
        xSemaphoreGive(mutex);
        TileKey kept[BROWSE_TERRAIN];
        for (int i = 0; i < count; i++) {
            kept[i] = tiles[i];
        }
        if (!runTerrainTiles(BROWSE_REGION, tiles, count, true)) {
            if (!terrainYielded) {
                terrainFailedMs = nowMs();
            }
            // Fetched again later (the next run skips the ones stored before it stopped)
            xSemaphoreTake(mutex, portMAX_DELAY);
            for (int i = 0; i < count && browseTerrainCount < BROWSE_TERRAIN; i++) {
                bool known = false;
                for (int j = 0; j < browseTerrainCount && !known; j++) {
                    known = browseTerrain[j] == kept[i];
                }
                if (!known) {
                    browseTerrain[browseTerrainCount++] = kept[i];
                }
            }
            xSemaphoreGive(mutex);
        }
        releaseIdle();
    }
    // Downloaded regions without all their heights, once Terrain is on: one region at a time while the map is left alone
    if (!terrainSweep || !updatesAllowed || stopRequested) {
        return;
    }
    bool pending = false;
    for (int i = 0; i < store->regionCount() && updatesAllowed && !stopRequested; i++) {
        const char* name = store->regionName(i);
        if (strcmp(name, BROWSE_REGION) == 0) {
            continue;
        }
        MarkerJob marker { rootPath, name, false };
        io->run(IoPriority::Background, terrainMarkerJob, &marker);
        RegionMeta region {};
        if (marker.exists || !readMeta(name, region) || !region.isComplete() ||
            tileRange(region.box, TERRAIN_ZOOM).count() > MAX_TERRAIN_TILES) {
            continue;
        }
        snprintf(region.name, sizeof(region.name), "%s", name);
        if (!claimIdle()) {
            return;
        }
        logInfo("Heights: %s has no terrain.ok, its missing heights are fetched", name);
        const bool done = runTerrain(region, true);
        releaseIdle();
        if (!done) {
            // Input, a failure or a job: the sweep goes on later
            pending = true;
            if (updatesAllowed && !cancelRequested) {
                terrainFailedMs = nowMs();
            }
            break;
        }
    }
    terrainSweep = pending || !updatesAllowed;
}

void Downloader::browseProgress(int& done, int& total) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    done = browseDone;
    total = std::max(browseTotal, browseDone);
    xSemaphoreGive(mutex);
}

void Downloader::noteArrived(const TileKey& tile) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    if (arrivedCount < MAX_ARRIVED) {
        arrived[arrivedCount++] = tile;
    } else {
        allTilesChanged = true;
    }
    xSemaphoreGive(mutex);
    tilesChanged = true;
}

bool Downloader::findOutdated(char* region, size_t size) {
    for (int i = 0; i < store->regionCount(); i++) {
        const char* name = store->regionName(i);
        // Browsed tiles have no plan to walk through, they stay as they were fetched
        if (strcmp(name, BROWSE_REGION) == 0) {
            continue;
        }
        RegionMeta candidate {};
        if (readMeta(name, candidate) && candidate.isComplete() && candidate.version[0] != '\0' && strcmp(candidate.version, version) != 0) {
            snprintf(region, size, "%s", name);
            return true;
        }
    }
    return false;
}

namespace {

/** Which tiles of a region are already at the new version: maps/<region>/update.bin, so an update resumes. */
struct UpdateProgress {
    static constexpr uint32_t MAGIC = 0x5055'4D54; // "TMUP"
    static constexpr size_t HEADER = 4 + 4 + 48;
    char version[48];
    int64_t total;
    int64_t done;
    uint8_t* bits;

    bool isSet(int64_t index) const { return (bits[index >> 3] & (1u << (index & 7))) != 0; }
    void set(int64_t index) {
        if (!isSet(index)) {
            bits[index >> 3] = static_cast<uint8_t>(bits[index >> 3] | (1u << (index & 7)));
            done++;
        }
    }
    size_t bitBytes() const { return static_cast<size_t>((total + 7) / 8); }
};

struct ProgressJob {
    char path[PATH_MAX_LENGTH];
    UpdateProgress* progress;
    bool write;
};

bool progressJob(IoTask& io, void* context) {
    auto* job = static_cast<ProgressJob*>(context);
    UpdateProgress& progress = *job->progress;
    if (job->write) {
        const size_t size = UpdateProgress::HEADER + progress.bitBytes();
        auto* bytes = static_cast<uint8_t*>(largeAlloc(size));
        if (bytes == nullptr) {
            return false;
        }
        const uint32_t magic = UpdateProgress::MAGIC;
        const auto total = static_cast<uint32_t>(progress.total);
        memcpy(bytes, &magic, 4);
        memcpy(bytes + 4, &total, 4);
        memcpy(bytes + 8, progress.version, 48);
        memcpy(bytes + UpdateProgress::HEADER, progress.bits, progress.bitBytes());
        char temporary[PATH_MAX_LENGTH + 8];
        snprintf(temporary, sizeof(temporary), "%s.tmp", job->path);
        const bool ok = io.writeFile(job->path, temporary, bytes, size, false);
        largeFree(bytes);
        return ok;
    }
    FILE* file = fopen(job->path, "rb");
    if (file == nullptr) {
        return false;
    }
    uint8_t header[UpdateProgress::HEADER];
    bool ok = fread(header, 1, sizeof(header), file) == sizeof(header);
    uint32_t magic = 0;
    uint32_t total = 0;
    if (ok) {
        memcpy(&magic, header, 4);
        memcpy(&total, header + 4, 4);
        ok = magic == UpdateProgress::MAGIC && total == progress.total && memcmp(header + 8, progress.version, 48) == 0 &&
            fread(progress.bits, 1, progress.bitBytes(), file) == progress.bitBytes();
    }
    fclose(file);
    if (!ok) {
        memset(progress.bits, 0, progress.bitBytes());
    }
    return ok;
}

} // namespace

void Downloader::runUpdate(bool manual) {
    RegionMeta target {};
    if (!readMeta(current.region, target)) {
        setStatus(State::Failed, "Region information is missing");
        return;
    }
    snprintf(target.name, sizeof(target.name), "%s", current.region);
    if (!target.isComplete()) {
        // An unfinished download continues as a download, which fetches from the newest version anyway
        setStatus(State::Failed, "Finish the download first");
        return;
    }
    if (!wifiConnected()) {
        setStatus(State::Failed, "No Wi-Fi connection");
        return;
    }
    Connection& connection = connections[0];
    connection.owner = this;
    connection.index = 0;
    connection.body = &bodies[0];
    connection.received = 0;
    connection.lastFolder[0] = '\0';
    if (!ensureTileJson(connection)) {
        giveClient(connection);
        setStatus(State::Failed, "OpenFreeMap is not reachable");
        return;
    }
    if (strcmp(target.version, version) == 0) {
        giveClient(connection);
        setStatus(State::Done, "Region is up to date");
        return;
    }

    UpdateProgress progress {};
    snprintf(progress.version, sizeof(progress.version), "%s", version);
    progress.total = target.total;
    progress.bits = static_cast<uint8_t*>(largeAlloc(progress.bitBytes()));
    if (progress.bits == nullptr) {
        giveClient(connection);
        setStatus(State::Failed, "Out of memory");
        return;
    }
    memset(progress.bits, 0, progress.bitBytes());
    ProgressJob progressFile {};
    snprintf(progressFile.path, sizeof(progressFile.path), "%s/maps/%s/update.bin", rootPath, target.name);
    progressFile.progress = &progress;
    io->run(IoPriority::Background, progressJob, &progressFile);
    for (int64_t i = 0; i < progress.total; i++) {
        progress.done += progress.isSet(i) ? 1 : 0;
    }
    logInfo("Update %s from %s to %s: %d of %d tiles done", target.name, target.version, version, static_cast<int>(progress.done),
        static_cast<int>(progress.total));
    xSemaphoreTake(mutex, portMAX_DELAY);
    current.total = progress.total;
    current.done = progress.done;
    current.bytes = 0;
    const BoundingBox view = viewBox;
    const bool hasView = viewKnown;
    xSemaphoreGive(mutex);
    setStatus(State::Updating, manual ? "Updating" : "Updating in idle time");

    const DownloadPlan plan = target.plan();
    if (!loadOutlineTiles(target)) {
        largeFree(progress.bits);
        giveClient(connection);
        setStatus(State::Failed, outlineNoMemory ? "Not enough memory for the region list" : "Region outline is missing");
        return;
    }
    bool failedTile = false;
    int unsaved = 0;
    auto stopNow = [&] { return stopRequested || cancelRequested || (!manual && !updatesAllowed); };
    auto updateOne = [&](int64_t index) {
        TileKey tile {};
        if (index < 0 || index >= progress.total || progress.isSet(index) || !tileForIndex(target, index, tile)) {
            return true;
        }
        bool ok = false;
        int status = 0;
        for (int attempt = 1; attempt <= 2 && !ok && !stopRequested; attempt++) {
            takeClient(connection);
            ok = connection.client != nullptr && downloadTile(connection, target.name, tile, status);
            if (!ok) {
                dropClient(connection);
            }
        }
        if (!ok) {
            logError("Update: tile %d/%d/%d failed (HTTP %d)", tile.z, tile.x, tile.y, status);
            return false;
        }
        if (status != 200) {
            // The new version has nothing here any more: the old tile goes
            RemoveJob old {};
            if (tilePath(old.path, sizeof(old.path), rootPath, target.name, tile)) {
                io->run(IoPriority::Background, removeJob, &old);
            }
            if (preparedPath(old.path, sizeof(old.path), rootPath, target.name, tile)) {
                io->run(IoPriority::Background, removeJob, &old);
            }
        }
        progress.set(index);
        noteArrived(tile);
        xSemaphoreTake(mutex, portMAX_DELAY);
        current.done = progress.done;
        xSemaphoreGive(mutex);
        if (++unsaved >= SAVE_EVERY_TILES) {
            unsaved = 0;
            progressFile.write = true;
            io->run(IoPriority::Background, progressJob, &progressFile);
        }
        return true;
    };
    // The tiles in view first, from the lowest zoom level, then all the others in download order
    if (hasView && outlineTiles != nullptr) {
        for (int64_t index = 0; index < outlineCount && !stopNow() && !failedTile; index++) {
            const TileRange seen = tileRange(view, outlineTiles[index].z);
            const TileKey& tile = outlineTiles[index];
            if (tile.x >= seen.minX && tile.x <= seen.maxX && tile.y >= seen.minY && tile.y <= seen.maxY) {
                failedTile = !updateOne(index);
            }
        }
    } else if (hasView) {
        for (int z = plan.minZoom; z <= plan.maxZoom && !stopNow() && !failedTile; z++) {
            const TileRange area = tileRange(plan.box, z);
            const TileRange seen = tileRange(view, z);
            for (int y = std::max(area.minY, seen.minY); y <= std::min(area.maxY, seen.maxY) && !stopNow() && !failedTile; y++) {
                for (int x = std::max(area.minX, seen.minX); x <= std::min(area.maxX, seen.maxX) && !stopNow() && !failedTile; x++) {
                    failedTile = !updateOne(indexOfTile(plan, { z, x, y }));
                }
            }
        }
    }
    for (int64_t index = 0; index < progress.total && !stopNow() && !failedTile; index++) {
        failedTile = !updateOne(index);
    }
    giveClient(connection);
    bodies[0].release();
    freeOutlineTiles();

    if (progress.done >= progress.total) {
        snprintf(target.version, sizeof(target.version), "%s", version);
        time_t now = time(nullptr);
        struct tm local {};
        localtime_r(&now, &local);
        strftime(target.date, sizeof(target.date), "%Y-%m-%d", &local);
        writeMeta(target);
        store->setRegionVersion(target.name, target.version);
        RemoveJob done {};
        snprintf(done.path, sizeof(done.path), "%s", progressFile.path);
        io->run(IoPriority::Background, removeJob, &done);
        char message[64];
        snprintf(message, sizeof(message), "%s updated", target.name);
        setStatus(State::Done, message);
    } else {
        progressFile.write = true;
        io->run(IoPriority::Background, progressJob, &progressFile);
        if (failedTile) {
            setStatus(State::Failed, "Update failed, it continues later");
        } else {
            // Paused by input: no message, the pill just goes
            setStatus(State::Idle, "");
        }
    }
    largeFree(progress.bits);
}

void Downloader::runDelete() {
    setStatus(State::Deleting, "Deleting");
    if (strcmp(current.region, BROWSE_REGION) == 0) {
        // Browsing starts the region again with its next tile
        browseMetaReady = false;
    }
    RegionMeta target {};
    readMeta(current.region, target);
    snprintf(target.name, sizeof(target.name), "%s", current.region);

    // Other regions keep the tiles they skipped because this region had them
    auto* others = static_cast<RegionMeta*>(largeAlloc(sizeof(RegionMeta) * TileStore::MAX_REGIONS));
    auto* columns = static_cast<int*>(largeAlloc(sizeof(int) * MAX_DELETE_COLUMNS));
    if (others == nullptr || columns == nullptr) {
        largeFree(others);
        largeFree(columns);
        setStatus(State::Failed, "Out of memory");
        return;
    }
    int otherCount = 0;
    for (int i = 0; i < store->regionCount() && otherCount < TileStore::MAX_REGIONS; i++) {
        const char* name = store->regionName(i);
        if (strcmp(name, target.name) != 0 && readMeta(name, others[otherCount])) {
            snprintf(others[otherCount].name, sizeof(others[otherCount].name), "%s", name);
            otherCount++;
        }
    }

    // One column folder per job, so the tiles on screen are read in between
    for (int z = 0; z <= MAX_SOURCE_ZOOM && !cancelRequested; z++) {
        ColumnsJob list {};
        snprintf(list.folder, sizeof(list.folder), "%s/maps/%s/%d", rootPath, target.name, z);
        list.columns = columns;
        if (!io->run(IoPriority::Background, listColumnsJob, &list)) {
            continue;
        }
        for (int i = 0; i < list.count && !cancelRequested; i++) {
            DeleteColumnJob job { rootPath, target.name, z, columns[i], others, otherCount };
            io->run(IoPriority::Background, deleteColumnJob, &job);
        }
        RemoveJob folder {};
        snprintf(folder.path, sizeof(folder.path), "%s", list.folder);
        folder.folder = true;
        io->run(IoPriority::Background, removeJob, &folder);
    }
    largeFree(others);
    largeFree(columns);
    RemoveJob metaFile {};
    snprintf(metaFile.path, sizeof(metaFile.path), "%s/maps/%s/meta.json", rootPath, target.name);
    io->run(IoPriority::Background, removeJob, &metaFile);
    // What the search index left in the region: the stamp, the index, the update list, the block and scratch folders.
    // Otherwise the folder is not empty and stays behind.
    static const char* const LEFTOVER_FILES[] = { "search.inf", "search.idx", "search.tmp", "stamp.tmp", "update.bin", "terrain.ok", "terrain.tmp" };
    for (const char* leftover : LEFTOVER_FILES) {
        RemoveJob file {};
        snprintf(file.path, sizeof(file.path), "%s/maps/%s/%s", rootPath, target.name, leftover);
        io->run(IoPriority::Background, removeJob, &file);
    }
    static const char* const LEFTOVER_FOLDERS[] = { "search", "srch" };
    for (const char* leftover : LEFTOVER_FOLDERS) {
        RemoveJob inside {};
        snprintf(inside.path, sizeof(inside.path), "%s/maps/%s/%s", rootPath, target.name, leftover);
        io->run(IoPriority::Background, clearFolderJob, &inside);
        inside.folder = true;
        io->run(IoPriority::Background, removeJob, &inside);
    }
    RemoveJob folder {};
    snprintf(folder.path, sizeof(folder.path), "%s/maps/%s", rootPath, target.name);
    folder.folder = true;
    io->run(IoPriority::Background, removeJob, &folder);
    // A region's terrain.ok may count on heights stored in the deleted one: all are checked again
    for (int i = 0; i < store->regionCount(); i++) {
        RemoveJob marker {};
        snprintf(marker.path, sizeof(marker.path), "%s/maps/%s/terrain.ok", rootPath, store->regionName(i));
        io->run(IoPriority::Background, removeJob, &marker);
    }
    terrainSweep = terrainWithDownloads;
    allTilesChanged = true;
    tilesChanged = true;
    setStatus(State::Done, cancelRequested ? "Delete stopped" : "Region deleted");
}

} // namespace maps
