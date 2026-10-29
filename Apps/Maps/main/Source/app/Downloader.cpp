// SPDX-License-Identifier: GPL-3.0-or-later
#include "Downloader.h"
#include "RootCertificates.h"

#include "../platform/Platform.h"
#include "../tiles/TileJson.h"

#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <freertos/idf_additions.h>
#include <tactility/device.h>
#include <tactility/drivers/wifi.h>

#include <sys/stat.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <initializer_list>

namespace maps {

namespace {

constexpr uint32_t CONTROL_STACK = 8192;
constexpr uint32_t CONNECTION_STACK = 6144;
constexpr UBaseType_t TASK_PRIORITY = 3;
/**
 * Network tasks run on core 1. While a TLS handshake (2-3 s of CPU) ran on core 0, the map loop and the render lane
 * there made no progress for its whole length; on core 1 no stall was seen (docs/upstream/https-stall.md).
 */
constexpr BaseType_t NETWORK_CORE = 1;

/**
 * Starts a network task with its stack in PSRAM, which keeps about 28 KB of internal RAM free for Wi-Fi, TLS and
 * the display. These tasks never touch flash or the SD card themselves. The task ends by suspending itself after
 * signalling its owner, who then deletes it with endNetworkTask().
 */
bool startNetworkTask(TaskFunction_t function, const char* name, uint32_t stack, void* context, TaskHandle_t* task) {
    return xTaskCreatePinnedToCoreWithCaps(function, name, stack, context, TASK_PRIORITY, task, NETWORK_CORE, MALLOC_CAP_SPIRAM) == pdPASS;
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

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
    if (event->event_id == HTTP_EVENT_ON_CONNECTED) {
        connectedAtMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    }
    if (event->event_id == HTTP_EVENT_ON_DATA && event->user_data != nullptr) {
        auto* body = static_cast<ByteWriter*>(event->user_data);
        if (body->size() + static_cast<size_t>(event->data_len) <= MAX_TILE_BYTES) {
            body->putBytes(event->data, static_cast<size_t>(event->data_len));
        } else {
            body->reserve(MAX_TILE_BYTES + 1);
            body->putBytes(event->data, 1);
        }
    }
    return ESP_OK;
}

esp_http_client_handle_t createClient(const char* url, ByteWriter* sink) {
    esp_http_client_config_t config = {};
    config.url = url;
    config.user_agent = USER_AGENT;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.keep_alive_enable = true;
    config.cert_pem = ROOT_CERTIFICATES;
    config.cert_len = sizeof(ROOT_CERTIFICATES);
    config.event_handler = onHttpEvent;
    config.user_data = sink;
    config.buffer_size = 4096;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != nullptr) {
        // Tiles are stored as the server compressed them
        esp_http_client_set_header(client, "Accept-Encoding", "gzip");
    }
    return client;
}

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
    const ByteWriter* body;
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
    const bool ok = io.writeFile(path, temporary, job->body->bytes(), job->body->size(), true);
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
    if (mutex == nullptr || wake == nullptr || stopped == nullptr || connectionsDone == nullptr || tileJsonLock == nullptr ||
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
    for (SemaphoreHandle_t* handle : { &mutex, &wake, &stopped, &connectionsDone, &tileJsonLock, &browseWake, &browseStopped }) {
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
        xSemaphoreTake(wake, portMAX_DELAY);
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
        }
        xSemaphoreTake(mutex, portMAX_DELAY);
        const bool more = job != Job::None;
        busy = more;
        xSemaphoreGive(mutex);
    }
}

bool Downloader::fetchTileJson() {
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
    const int64_t initUs = esp_timer_get_time();
    esp_http_client_handle_t client = createClient(TILEJSON_URL, nullptr);
    if (client == nullptr) {
        return false;
    }
    // The TileJSON is read as plain text
    esp_http_client_delete_header(client, "Accept-Encoding");
    bool ok = false;
    const int64_t startUs = esp_timer_get_time();
    connectedAtMs = 0;
    const esp_err_t opened = esp_http_client_open(client, 0);
    const int64_t openedUs = esp_timer_get_time();
    const int64_t length = opened == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
    const int64_t headersUs = esp_timer_get_time();
    const int status = esp_http_client_get_status_code(client);
    if (opened != ESP_OK || length < 0 || status != 200) {
        logError("TileJSON request failed: %s, HTTP %d", esp_err_to_name(opened), status);
    } else {
        // The tile URL comes first in OpenFreeMap's TileJSON, the long list of layer fields after it
        char* text = static_cast<char*>(largeAlloc(8192));
        if (text != nullptr) {
            int received = 0;
            while (received < 8191) {
                const int count = esp_http_client_read(client, text + received, 8191 - received);
                if (count <= 0) {
                    break;
                }
                received += count;
            }
            text[received] = '\0';
            if (timing) {
                const int64_t readUs = esp_timer_get_time();
                logInfo("Timing https: init %u ms, start t=%u ms, connected +%u ms (DNS, TCP, TLS), open +%u, headers +%u, body +%u (%d bytes)",
                    msSince(initUs, startUs), static_cast<unsigned>(startUs / 1000), connectedAtMs > 0 ? connectedAtMs - static_cast<uint32_t>(startUs / 1000) : 0,
                    msSince(startUs, openedUs), msSince(startUs, headersUs), msSince(startUs, readUs), received);
            }
            ok = parseTileJson(text, urlTemplate, sizeof(urlTemplate), version, sizeof(version));
            if (!ok) {
                logError("TileJSON has no usable tile URL (%d bytes read)", received);
            }
            largeFree(text);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
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
    const esp_err_t result = esp_http_client_perform(client);
    status = esp_http_client_get_status_code(client);
    const size_t size = connection.body->size();
    if (result != ESP_OK || connection.body->failed() || size > MAX_TILE_BYTES || (status != 200 && status != 204 && status != 404)) {
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
    if (!stored) {
        return false;
    }
    connection.received += static_cast<uint64_t>(size);
    return true;
}

bool Downloader::claimIndex(int64_t& index, int connectionIndex) {
    xSemaphoreTake(mutex, portMAX_DELAY);
    const bool available = nextIndex < meta.total && !failed && !cancelRequested;
    if (available) {
        index = nextIndex++;
        activeIndex[connectionIndex] = index;
    }
    xSemaphoreGive(mutex);
    return available;
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
        if (!tileAtIndex(meta.plan(), index, tile)) {
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
            if (connection.client == nullptr) {
                connection.client = createClient(TILEJSON_URL, connection.body);
            }
            ok = connection.client != nullptr && downloadTile(connection, meta.name, tile, status);
            if (!ok) {
                logError("[%d] Tile %d/%d/%d failed (HTTP %d), attempt %d", connection.index, tile.z, tile.x, tile.y, status, attempt);
                if (connection.client != nullptr) {
                    esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(connection.client));
                    connection.client = nullptr;
                }
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
    if (connection.client != nullptr) {
        esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(connection.client));
        connection.client = nullptr;
    }
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
    if (!wifiConnected()) {
        setStatus(State::Failed, "No Wi-Fi connection");
        return;
    }
    setStatus(State::Connecting, "Connecting to OpenFreeMap");
    logMemory("download start");
    if (!ensureTileJson()) {
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
    writeMeta(meta);
    store->setRegionVersion(meta.name, meta.version);
    tilesChanged = true;

    const uint32_t seconds = (nowMs() - start) / 1000;
    char message[64];
    if (meta.bytes > MAX_DOWNLOAD_BYTES) {
        setStatus(State::Failed, "Stopped at the 50 MB limit");
    } else if (failed) {
        setStatus(State::Failed, "Download failed, it continues next time");
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

bool Downloader::ensureTileJson() {
    // The region download and the browse task may both need it first
    xSemaphoreTake(tileJsonLock, portMAX_DELAY);
    const bool ok = urlTemplate[0] != '\0' || fetchTileJson();
    xSemaphoreGive(tileJsonLock);
    return ok;
}

bool Downloader::isOnline() {
    return wifiConnected();
}

void Downloader::browse(const TileKey* tiles, int count) {
    if (browseTask == nullptr) {
        return;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    bool added = false;
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
        }
        browseQueue[browseCount++] = tiles[i];
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
    int status = 0;
    bool ok = false;
    for (int attempt = 1; attempt <= 2 && !ok && !stopRequested; attempt++) {
        if (connection.client == nullptr) {
            connection.client = createClient(TILEJSON_URL, connection.body);
        }
        ok = connection.client != nullptr && downloadTile(connection, BROWSE_REGION, tile, status);
        if (!ok && connection.client != nullptr) {
            esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(connection.client));
            connection.client = nullptr;
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
    logInfo("Browse: tile %d/%d/%d, %u bytes", tile.z, tile.x, tile.y, static_cast<unsigned>(connection.received - before));
    if (++browseUnsaved >= 5) {
        browseUnsaved = 0;
        writeMeta(browseMeta);
    }
    tilesChanged = true;
    return true;
}

void Downloader::browseLoop() {
    browseConnection = { this, CONNECTIONS, nullptr, &browseBody, 0, nullptr, "" };
    while (!stopRequested) {
        xSemaphoreTake(browseWake, portMAX_DELAY);
        if (warmUp.exchange(false) && wifiConnected()) {
            ensureTileJson();
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
            if (!wifiConnected() || !ensureTileJson()) {
                // Offline: the map says so, the requests are dropped and come again with the next view
                xSemaphoreTake(mutex, portMAX_DELAY);
                browseCount = 0;
                xSemaphoreGive(mutex);
                break;
            }
            browsing = true;
            if (!fetchBrowseTile(tile)) {
                // A failing server is not asked again at once
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
        }
        browsing = false;
        browseBody.release();
        if (browseUnsaved > 0) {
            browseUnsaved = 0;
            writeMeta(browseMeta);
        }
    }
    if (browseConnection.client != nullptr) {
        esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(browseConnection.client));
        browseConnection.client = nullptr;
    }
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
    tilesChanged = true;
    setStatus(State::Done, cancelRequested ? "Delete stopped" : "Region deleted");
}

} // namespace maps
