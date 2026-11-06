// SPDX-License-Identifier: GPL-3.0-or-later
#include "Downloader.h"
#include "RootCertificates.h"

#include "../platform/Platform.h"
#include "../mvt/PlaceFinder.h"
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

namespace maps {

namespace {

constexpr uint32_t CONTROL_STACK = 8192;
constexpr uint32_t CONNECTION_STACK = 6144;
constexpr UBaseType_t TASK_PRIORITY = 3;
/** Tiles at this zoom level and below are downloaded one at a time. */
constexpr int ALONE_MAX_ZOOM = 7;
/** Idle keep-alive connections are closed after this long, a new one costs a full TLS handshake. */
constexpr uint32_t IDLE_CLOSE_MS = 60000;
/** How often the network tasks look for idle connections to close. */
constexpr TickType_t SWEEP_TICKS = pdMS_TO_TICKS(5000);

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
    xSemaphoreTake(handshakeLock, pdMS_TO_TICKS(HTTP_TIMEOUT_MS));
    connection.handshaking = true;
    handshakes++;
}

void Downloader::endHandshake(Connection& connection) {
    if (connection.handshaking) {
        connection.handshaking = false;
        handshakes--;
        xSemaphoreGive(handshakeLock);
    }
}

esp_err_t Downloader::perform(Connection& connection) {
    auto* client = static_cast<esp_http_client_handle_t>(connection.client);
    const bool wasConnected = connection.connected;
    if (!wasConnected) {
        beginHandshake(connection);
    }
    connection.requestStartMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    connection.connectMs = 0;
    const esp_err_t result = esp_http_client_perform(client);
    endHandshake(connection);
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
};

bool onCardJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<OnCardJob*>(context);
    char path[PATH_MAX_LENGTH];
    job->inThisRegion = tilePath(path, sizeof(path), job->root, job->region, job->tile) && fileExists(path);
    if (job->inThisRegion) {
        return true;
    }
    // A tile of an overlapping region is shared instead of downloaded again
    for (int i = 0; i < job->store->regionCount(); i++) {
        const char* other = job->store->regionName(i);
        if (strcmp(other, job->region) != 0 && tilePath(path, sizeof(path), job->root, other, job->tile) && fileExists(path)) {
            return true;
        }
    }
    return false;
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
    if (preparedPath(prepared, sizeof(prepared), job->root, job->region, job->tile) && fileExists(prepared)) {
        remove(prepared);
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
    if (handshakeLock == nullptr || mutex == nullptr || wake == nullptr || stopped == nullptr || connectionsDone == nullptr || tileJsonLock == nullptr ||
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
    for (SemaphoreHandle_t* handle : { &mutex, &wake, &stopped, &connectionsDone, &tileJsonLock, &browseWake, &browseStopped, &handshakeLock }) {
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
        logError("Region catalog %s not readable", catalogPath);
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

void Downloader::setStatus(State state, const char* message) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    current.state = state;
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
        if (xSemaphoreTake(wake, SWEEP_TICKS) != pdTRUE) {
            closeIdleClients(IDLE_CLOSE_MS);
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
    OnCardJob job { rootPath, store, region, tile, false };
    const bool found = io->run(IoPriority::Background, onCardJob, &job);
    inThisRegion = job.inThisRegion;
    return found;
}

bool Downloader::downloadTile(Connection& connection, const char* region, const TileKey& tile, int& status) {
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
    // Card work waits until the map has been left alone for a moment, writes hold the display's shared bus
    while (!idle && !stopRequested && !cancelRequested) {
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
        bytes += connections[i].received;
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
    for (int i = 0; i < CONNECTIONS; i++) {
        bytes += connections[i].received;
    }
    copy.bytes = meta.bytes + bytes;
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
        bool inThisRegion = false;
        if (tileOnCard(meta.name, tile, inThisRegion)) {
            finishIndex(connection.index);
            continue;
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
        // The limit holds for the real size too, the estimate can be lower for dense city centres
        xSemaphoreTake(mutex, portMAX_DELAY);
        uint64_t bytes = meta.bytes;
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
        setStatus(State::Failed, "Region outline is missing");
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
    for (int i = 0; i < CONNECTIONS; i++) {
        meta.bytes += connections[i].received;
        connections[i].received = 0;
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
    allTilesChanged = true;
    tilesChanged = true;

    const uint32_t seconds = (nowMs() - start) / 1000;
    char message[64];
    if (meta.bytes > MAX_DOWNLOAD_BYTES) {
        setStatus(State::Failed, "Stopped at the 50 MB limit");
    } else if (failed) {
        setStatus(State::Failed, "Download failed, it continues later");
    } else if (cancelRequested) {
        setStatus(State::Done, "Download paused");
    } else {
        snprintf(message, sizeof(message), "Downloaded %d KB in %u s", static_cast<int>(meta.bytes / 1024), static_cast<unsigned>(seconds));
        setStatus(State::Done, message);
    }
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
        ok = connection.client != nullptr && downloadTile(connection, BROWSE_REGION, tile, status);
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
        giveClient(browseConnection);
        browseBody.release();
        if (browseUnsaved > 0) {
            browseUnsaved = 0;
            writeMeta(browseMeta);
        }
    }
    dropClient(browseConnection);
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
        setStatus(State::Failed, "Region outline is missing");
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
    RemoveJob folder {};
    snprintf(folder.path, sizeof(folder.path), "%s/maps/%s", rootPath, target.name);
    folder.folder = true;
    io->run(IoPriority::Background, removeJob, &folder);
    allTilesChanged = true;
    tilesChanged = true;
    setStatus(State::Done, cancelRequested ? "Delete stopped" : "Region deleted");
}

} // namespace maps
