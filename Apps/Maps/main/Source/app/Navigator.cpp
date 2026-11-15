// SPDX-License-Identifier: GPL-3.0-or-later
#include "Navigator.h"

#include "TileWorker.h"

#include "../platform/Platform.h"
#include "../route/RouteSplice.h"
#include "../route/RouteTrack.h"
#include "../tiles/TileStore.h"

#include <esp_heap_caps.h>
#include <freertos/idf_additions.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr uint32_t TASK_STACK = 12 * 1024;
constexpr UBaseType_t TASK_PRIORITY = 3;
constexpr char ROUTE_MAGIC[4] = { 'T', 'M', 'R', 'T' };
constexpr uint16_t ROUTE_FILE_VERSION = 1;
/** Left free for the rest of the app while a search runs. */
constexpr size_t KEEP_FREE_BYTES = 1024 * 1024;

struct GraphReadJob {
    TileStore* store;
    TileKey tile;
    FileBuffer* buffer;
    bool found;
    bool yielded;
    /** The region the graph was found in last time, tried first. */
    int* lastRegion;
    uint32_t findMs;
    uint32_t readMs;
    /** The original is on the card, in this region, but no graph was made from it yet. */
    bool noGraph = false;
    int region = -1;
};

bool graphReadJob(IoTask& io, void* context) {
    auto* job = static_cast<GraphReadJob*>(context);
    TileFile file {};
    const uint32_t started = nowMs();
    job->found = job->store->find(job->tile, file) && file.tile == job->tile;
    job->findMs = nowMs() - started;
    if (!job->found) {
        return false;
    }
    // Overlapping regions can hold the same tile: the first one whose graph is made already, the region that had the
    // last tile's graph first (a route stays mostly in one region)
    char path[PATH_MAX_LENGTH];
    const int count = job->store->regionCount();
    for (int n = -1; n < count; n++) {
        const int i = n < 0 ? *job->lastRegion : n;
        // Only regions whose area has the tile: a file lookup in a large region folder costs hundreds of ms
        if (i < 0 || i >= count || (n >= 0 && i == *job->lastRegion) || !job->store->mayHave(i, job->tile)) {
            continue;
        }
        if (preparedFilePath(path, sizeof(path), job->store->root(), job->store->regionName(i), job->tile, PreparedFile::Graph) && fileExists(path)) {
            const uint32_t reading = nowMs();
            const bool ok = io.readFile(path, *job->buffer);
            job->readMs = nowMs() - reading;
            job->yielded = io.yielded();
            if (ok) {
                *job->lastRegion = i;
            }
            return ok;
        }
    }
    job->noGraph = true;
    job->region = file.region;
    return false;
}

void putU16(GrowArray<uint8_t>& out, uint16_t value) {
    out.push(static_cast<uint8_t>(value));
    out.push(static_cast<uint8_t>(value >> 8));
}

void putU32(GrowArray<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; i++) {
        out.push(static_cast<uint8_t>(value >> (8 * i)));
    }
}

uint32_t readU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

} // namespace

/** Reads graph files through the I/O task from the region that holds the tile. */
class Navigator::CardSource final : public GraphSource {
public:
    CardSource(IoTask& io, TileStore& store, TileWorker& worker) : io(io), store(store), worker(worker) {}
    ~CardSource() override = default;
    bool readGraph(const TileKey& tile, GrowArray<uint8_t>& file) override {
        GraphReadJob job { &store, tile, &buffer, false, false, &lastRegion, 0, 0 };
        const uint32_t started = nowMs();
        bool ok = io.run(IoPriority::Visible, graphReadJob, &job);
        if (nowMs() - started > 500) {
            logInfo("Route: graph of %d/%d/%d read in %u ms (find %u, read %u, region %d, %u bytes)", tile.z, tile.x, tile.y,
                static_cast<unsigned>(nowMs() - started), static_cast<unsigned>(job.findMs), static_cast<unsigned>(job.readMs), lastRegion,
                static_cast<unsigned>(buffer.size()));
        }
        // A read that gave way to tiles on screen is read again
        for (int attempt = 0; !ok && job.yielded && attempt < 20; attempt++) {
            job.yielded = false;
            ok = io.run(IoPriority::Visible, graphReadJob, &job);
        }
        if (!ok && job.noGraph && prepareLeftMs > 0 && worker.prepareForRoute(tile, job.region)) {
            // On the card, but its graph is not made yet (a region just downloaded, or the z10 tiles of the
            // main-road layer after an update): made now, and the search waits for it. Not without end: a route
            // through a whole new region would wait minutes, so after PREPARE_MS in all the tiles count as missing
            // (the idle time prepares them, and a later search has them)
            const uint32_t waitStart = nowMs();
            while (worker.isPreparingTile(tile) && nowMs() - waitStart < std::min<uint32_t>(prepareLeftMs, PREPARE_TILE_MS)) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            const uint32_t waited = nowMs() - waitStart;
            prepareLeftMs = waited < prepareLeftMs ? prepareLeftMs - waited : 0;
            prepared++;
            job.noGraph = false;
            ok = io.run(IoPriority::Visible, graphReadJob, &job);
            logInfo("Route: graph of %d/%d/%d made for the search in %u ms%s", tile.z, tile.x, tile.y, static_cast<unsigned>(waited), ok ? "" : ", not there");
        }
        if (!ok || !file.resize(buffer.size())) {
            readFailures++;
            logInfo("Route: no graph for %d/%d/%d (%s, yielded %d, %u bytes short, largest free block %u KB)", tile.z, tile.x, tile.y,
                job.found ? "file not read" : "no tile", job.yielded ? 1 : 0, static_cast<unsigned>(buffer.lastShortfall()),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
            return false;
        }
        // The buffer stays for the next tile: allocating it again for each tile left PSRAM in pieces
        memcpy(file.begin(), buffer.bytes(), buffer.size());
        return true;
    }
    bool memoryShort() override {
        // The map draws tiles and places labels meanwhile: it keeps at least this much
        return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < 256 * 1024 || heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < 768 * 1024;
    }
    int readFailures = 0;
    /** Graphs made because the search asked for them. */
    int prepared = 0;

private:
    /** The longest a search waits for graphs to be made, in all and for one tile. */
    static constexpr uint32_t PREPARE_MS = 60000;
    static constexpr uint32_t PREPARE_TILE_MS = 10000;
    uint32_t prepareLeftMs = PREPARE_MS;
    IoTask& io;
    TileStore& store;
    TileWorker& worker;
    FileBuffer buffer;
    int lastRegion = -1;
};

bool Navigator::start(IoTask& ioTask, TileStore& tileStore, TileWorker& tileWorker, const char* dataPath) {
    io = &ioTask;
    store = &tileStore;
    worker = &tileWorker;
    snprintf(routePath, sizeof(routePath), "%s/route.bin", dataPath);
    wake = xSemaphoreCreateBinary();
    mutex = xSemaphoreCreateMutex();
    stopped = xSemaphoreCreateBinary();
    if (wake == nullptr || mutex == nullptr || stopped == nullptr) {
        return false;
    }
    // The route of the last session
    if (loadRoute()) {
        logInfo("Route loaded: %s, %u points, %.0f m", routeProfileName(routes[current].profile), static_cast<unsigned>(routes[current].points.size()),
            routes[current].lengthM);
    }
    if (xTaskCreatePinnedToCoreWithCaps(taskMain, "maps-route", TASK_STACK, this, TASK_PRIORITY, &task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) != pdPASS) {
        task = nullptr;
        return false;
    }
    if (routeReady.load()) {
        // A route request coming first makes its own side roads
        job = Job::Sides;
        xSemaphoreGive(wake);
    }
    return true;
}

void Navigator::stop() {
    if (task != nullptr) {
        // Quit first, then cancel: runSides() resets the cancel and then looks for Quit
        job = Job::Quit;
        search.cancel();
        xSemaphoreGive(wake);
        xSemaphoreTake(stopped, portMAX_DELAY);
        vTaskDeleteWithCaps(task);
        task = nullptr;
    }
    for (SemaphoreHandle_t* handle : { &wake, &mutex, &stopped }) {
        if (*handle != nullptr) {
            vSemaphoreDelete(*handle);
            *handle = nullptr;
        }
    }
}

void Navigator::lock() {
    if (mutex != nullptr) {
        xSemaphoreTake(mutex, portMAX_DELAY);
    }
}

void Navigator::unlock() {
    if (mutex != nullptr) {
        xSemaphoreGive(mutex);
    }
}

bool Navigator::request(double fromLat, double fromLon, double toLat, double toLon, RouteProfile profile) {
    if (task == nullptr || currentState == State::Searching) {
        return false;
    }
    requestFrom[0] = fromLat;
    requestFrom[1] = fromLon;
    requestTo[0] = toLat;
    requestTo[1] = toLon;
    requestProfile = profile;
    requestProgress = -1.0f;
    currentState = State::Searching;
    job = Job::Route;
    xSemaphoreGive(wake);
    return true;
}

bool Navigator::requestRejoin(double fromLat, double fromLon, double toLat, double toLon, RouteProfile profile, float progressM) {
    if (task == nullptr || currentState == State::Searching) {
        return false;
    }
    requestFrom[0] = fromLat;
    requestFrom[1] = fromLon;
    requestTo[0] = toLat;
    requestTo[1] = toLon;
    requestProfile = profile;
    requestProgress = progressM;
    currentState = State::Searching;
    job = Job::Route;
    xSemaphoreGive(wake);
    return true;
}

bool Navigator::runRejoin(GraphSource& source, Route& next) {
    // "Clear route" on the main task can empty the current route at any time: what is needed of it is read under the
    // lock, and the splice is made under the lock only if the route has not changed since
    // Far enough ahead that the way back is no U-turn, near enough that the search is short
    const float ahead = requestProfile == RouteProfile::Walk ? 400.0f : 1200.0f;
    const RoutePoint from = routePointFrom(requestFrom[0], requestFrom[1]);
    lock();
    const uint32_t generation = changes.load();
    const Route& current0 = routes[current];
    const size_t target = current0.isEmpty() ? SIZE_MAX : rejoinTarget(current0, from, requestProgress, ahead, 1000.0f);
    const RoutePoint join = target != SIZE_MAX ? current0.points[target] : RoutePoint {};
    const RoutePoint destination = current0.to;
    unlock();
    if (target == SIZE_MAX) {
        return false;
    }
    double targetLat;
    double targetLon;
    routePointTo(join, targetLat, targetLon);
    Route piece;
    RouteStats pieceStats {};
    const RouteResult found = search.find(source, requestFrom[0], requestFrom[1], targetLat, targetLon, requestProfile, piece, &pieceStats);
    if (found != RouteResult::Found) {
        rejoinCancelled = found == RouteResult::Cancelled;
        logInfo("Route rejoin: %s in %u ms%s", routeResultText(found), pieceStats.totalMs, rejoinCancelled ? "" : ", searching to the destination");
        return false;
    }
    // A way back that is far longer than the straight line is no way back (a river, a motorway in between)
    const float dx = static_cast<float>(join.x - from.x);
    const float dy = static_cast<float>(join.y - from.y);
    const float straight = std::sqrt(dx * dx + dy * dy) * static_cast<float>(routeMetresPerUnit(from.y));
    // Nor is one that leads away from the destination (the position is already past that part of the old route)
    bool spliced = false;
    bool changed = false;
    if (piece.lengthM <= 3.0f * straight + 500.0f && rejoinLeadsOn(from, join, destination, piece.lengthM)) {
        lock();
        changed = changes.load() != generation;
        spliced = !changed && spliceRoute(piece, routes[current], target, next);
        rejoinGeneration = generation;
        unlock();
    }
    if (changed) {
        // The route was cleared or replaced meanwhile: nothing is restored, and no search to the old goal follows
        logInfo("Route rejoin: the route changed during the search, dropped");
        next.clear();
        rejoinCancelled = true;
        return false;
    }
    if (!spliced) {
        logInfo("Route rejoin: %.0f m for %.0f m in a straight line, or away from the destination: searching to the destination",
            piece.lengthM, straight);
        next.clear();
        return false;
    }
    stats = pieceStats;
    logInfo("Route rejoin: %.0f m back to the route, %.0f m in all, %u ms (settled %u, tiles %u)", piece.lengthM, next.lengthM, pieceStats.totalMs,
        pieceStats.settled, pieceStats.tilesRead);
    return true;
}

bool Navigator::bench(const char* pairsPath, RouteProfile profile) {
    if (task == nullptr || currentState == State::Searching) {
        return false;
    }
    snprintf(benchPath, sizeof(benchPath), "%s", pairsPath);
    requestProfile = profile;
    currentState = State::Searching;
    job = Job::Bench;
    xSemaphoreGive(wake);
    return true;
}

bool Navigator::requestTrack(const char* gpxPath) {
    if (task == nullptr || currentState == State::Searching) {
        return false;
    }
    snprintf(benchPath, sizeof(benchPath), "%s", gpxPath);
    currentState = State::Searching;
    job = Job::Track;
    xSemaphoreGive(wake);
    return true;
}

RouteResult Navigator::takeResult() {
    const RouteResult value = result;
    if (currentState == State::Done || currentState == State::Failed) {
        currentState = State::Idle;
    }
    return value;
}

void Navigator::clearRoute() {
    lock();
    routes[current].clear();
    routeReady = false;
    changes++;
    unlock();
    auto remove = [&]() {
        removeFile(routePath);
        return true;
    };
    runOnCard(*io, remove);
}

void Navigator::taskMain(void* context) {
    static_cast<Navigator*>(context)->loop();
}

void Navigator::loop() {
    while (true) {
        xSemaphoreTake(wake, portMAX_DELAY);
        const Job next = job.exchange(Job::None);
        if (next == Job::Quit) {
            break;
        }
        if (next == Job::Route) {
            runRoute();
        } else if (next == Job::Sides) {
            runSides();
        } else if (next == Job::Bench) {
            runBench();
        } else if (next == Job::Track) {
            runTrack();
        }
    }
    xSemaphoreGive(stopped);
    vTaskSuspend(nullptr);
}

bool Navigator::takeMemory() {
    worker->setRouting(true);
    // The node table is one block of up to 1 MB: the tile cache makes room for it, then the search takes what is free
    // Down to 6 slots: enough for the tiles on screen while the user waits for the route
    worker->relieveMemory(1100 * 1024, SEARCH_BYTES + KEEP_FREE_BYTES, 6);
    const size_t free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t budget = std::min(SEARCH_BYTES, free > KEEP_FREE_BYTES ? free - KEEP_FREE_BYTES : 0);
    if (budget < 512 * 1024 || !search.begin(budget)) {
        logError("Route: no memory for a search (%u KB free, largest block %u KB)", static_cast<unsigned>(free / 1024),
            static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
        search.end();
        worker->restoreMemory();
        worker->setRouting(false);
        return false;
    }
    logInfo("Route: search memory %u KB, %u node states", static_cast<unsigned>(budget / 1024), static_cast<unsigned>(search.nodeCapacity()));
    return true;
}

void Navigator::giveMemory() {
    search.end();
    worker->restoreMemory();
    worker->setRouting(false);
}

void Navigator::runRoute() {
    CardSource source(*io, *store, *worker);
    // Memory may be short during a download: waits up to 10 s for it
    bool memory = false;
    for (int attempt = 0; attempt < 10 && !(memory = takeMemory()); attempt++) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    if (!memory) {
        result = RouteResult::NoMemory;
        currentState = State::Failed;
        return;
    }
    Route& next = routes[1 - current];
    rejoinCancelled = false;
    rejoined = requestProgress >= 0.0f && runRejoin(source, next);
    // A stopped search (the app closes, another mode was chosen) is not followed by the long one
    const RouteResult found = rejoined ? RouteResult::Found
        : (rejoinCancelled ? RouteResult::Cancelled
            : search.find(source, requestFrom[0], requestFrom[1], requestTo[0], requestTo[1], requestProfile, next, &stats));
    giveMemory();
    logInfo("Route %s: %s, %.0f m, %.0f s, %u points, %u steps; %u ms (reading %u ms), settled %u, tiles %u (%u missing), peak %u KB%s",
        routeProfileName(requestProfile), routeResultText(found), next.lengthM, next.seconds, static_cast<unsigned>(next.points.size()),
        static_cast<unsigned>(next.steps.size()), stats.totalMs, stats.loadMs, stats.settled, stats.tilesRead, stats.tilesMissing, stats.peakBytes / 1024,
        stats.passes > 1 ? ", second pass toward the goal" : "");
    result = found;
    if (found == RouteResult::Found) {
        lock();
        if (rejoined && changes.load() != rejoinGeneration) {
            // "Clear route" came between the splice and here: the cleared route is not brought back
            unlock();
            next.clear();
            result = RouteResult::Cancelled;
            currentState = State::Failed;
            return;
        }
        if (rejoined) {
            // The side roads of the old route go on being used: most of the route is the same, and working them out
            // again reads every tile along it (5 to 6 s of card time after each new route, 14 Nov)
            next.sides.swap(routes[current].sides);
        }
        current = 1 - current;
        routeReady = true;
        changes++;
        unlock();
        saveRoute(routes[current]);
        routes[1 - current].clear();
        currentState = State::Done;
        // The roads beside the route, for the follower's edge rule: after the route is shown, not before
        if (!rejoined) {
            runSides();
        }
    } else {
        next.clear();
        currentState = State::Failed;
    }
}

void Navigator::runTrack() {
    // The file is read in chunks, with the card free for the map in between, and never held whole
    constexpr size_t CHUNK = 8192;
    struct Chunk {
        const char* path;
        long offset;
        char* buffer;
        size_t count;
        long fileSize;
    };
    auto* buffer = static_cast<char*>(largeAlloc(CHUNK));
    void* memory = largeAlloc(sizeof(TrackRouteBuilder));
    auto* builder = memory != nullptr ? new (memory) TrackRouteBuilder() : nullptr;
    Route& next = routes[1 - current];
    bool ok = buffer != nullptr && builder != nullptr;
    bool read = false;
    const uint32_t started = nowMs();
    if (ok) {
        GpxReader reader(*builder);
        Chunk chunk { benchPath, 0, buffer, 0, -1 };
        auto readChunk = [&]() {
            FILE* file = fopen(chunk.path, "rb");
            if (file == nullptr) {
                return false;
            }
            bool fine = true;
            if (chunk.fileSize < 0) {
                fine = fseek(file, 0, SEEK_END) == 0;
                chunk.fileSize = fine ? ftell(file) : -1;
            }
            fine = fine && fseek(file, chunk.offset, SEEK_SET) == 0;
            chunk.count = fine ? fread(chunk.buffer, 1, CHUNK, file) : 0;
            fclose(file);
            return fine;
        };
        bool fed = true;
        read = runOnCard(*io, readChunk, IoPriority::Background);
        while (read && chunk.count > 0 && job.load() != Job::Quit) {
            if (!reader.feed(buffer, chunk.count)) {
                fed = false;
                break;
            }
            chunk.offset += static_cast<long>(chunk.count);
            if (chunk.offset >= chunk.fileSize) {
                break;
            }
            read = runOnCard(*io, readChunk, IoPriority::Background);
            vTaskDelay(1);
        }
        // A damaged or unfinished file (no closing tags), or the app closing (stop()), gives no route: the old one
        // stays, and nothing half-read is saved
        const bool finished = reader.finish();
        const bool quit = job.load() == Job::Quit;
        ok = read && fed && finished && !quit && builder->finish(next) && job.load() != Job::Quit;
        logInfo("Route from the GPX file %s: %s, %u points read, %u kept, %.0f m, %u ms", benchPath, ok ? "made" : "none",
            static_cast<unsigned>(builder->pointsRead()), static_cast<unsigned>(next.points.size()), next.lengthM, nowMs() - started);
    }
    if (builder != nullptr) {
        builder->~TrackRouteBuilder();
    }
    largeFree(memory);
    largeFree(buffer);
    if (ok) {
        result = RouteResult::Found;
        lock();
        current = 1 - current;
        routeReady = true;
        changes++;
        unlock();
        saveRoute(routes[current]);
        routes[1 - current].clear();
        currentState = State::Done;
    } else {
        next.clear();
        result = buffer == nullptr || builder == nullptr ? RouteResult::NoMemory : RouteResult::NoTrack;
        currentState = State::Failed;
    }
}

void Navigator::runSides() {
    // The route's side roads: worked out on a copy of its points, then given to the route if it is still the same
    search.resetCancel();
    if (job.load() == Job::Quit) {
        return;
    }
    GrowArray<RoutePoint> points;
    lock();
    const uint32_t generation = changes.load();
    const Route& route = routes[current];
    if (points.reserve(route.points.size())) {
        for (const RoutePoint& point : route.points) {
            points.push(point);
        }
    }
    unlock();
    if (points.size() < 2) {
        return;
    }
    CardSource source(*io, *store, *worker);
    GrowArray<RouteSide> sides;
    const uint32_t started = nowMs();
    const bool complete = search.addSides(source, points, sides);
    lock();
    if (complete && changes.load() == generation) {
        routes[current].sides.swap(sides);
    }
    // Counted under the lock: clearRoute() may change the route right after
    const auto sideCount = static_cast<unsigned>(routes[current].sides.size());
    unlock();
    logInfo("Route: %u side segments in %u ms", sideCount,
        static_cast<unsigned>(nowMs() - started));
}

void Navigator::runBench() {
    FileBuffer file;
    auto read = [&]() { return io->readFile(benchPath, file); };
    if (!runOnCard(*io, read)) {
        logError("Route bench: %s does not read", benchPath);
        result = RouteResult::NoPath;
        currentState = State::Failed;
        return;
    }
    if (!takeMemory()) {
        result = RouteResult::NoMemory;
        currentState = State::Failed;
        return;
    }
    CardSource source(*io, *store, *worker);
    Route& scratch = routes[1 - current];
    const int firstProfile = requestProfile == RouteProfile::Count ? 0 : static_cast<int>(requestProfile);
    const int lastProfile = requestProfile == RouteProfile::Count ? 2 : firstProfile;
    for (int p = firstProfile; p <= lastProfile; p++) {
        const auto profile = static_cast<RouteProfile>(p);
        uint32_t found = 0;
        uint32_t tooLong = 0;
        uint32_t count = 0;
        uint64_t ms = 0;
        uint32_t maxMs = 0;
        uint64_t settled = 0;
        uint32_t maxSettled = 0;
        uint64_t tiles = 0;
        uint32_t maxTiles = 0;
        uint32_t peak = 0;
        const char* cursor = reinterpret_cast<const char*>(file.bytes());
        const char* end = cursor + file.size();
        while (cursor < end) {
            double a;
            double b;
            double c;
            double d;
            double cost;
            int studySettled;
            char line[160];
            const char* newline = static_cast<const char*>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
            const size_t length = std::min<size_t>((newline != nullptr ? newline : end) - cursor, sizeof(line) - 1);
            memcpy(line, cursor, length);
            line[length] = '\0';
            cursor = newline != nullptr ? newline + 1 : end;
            if (sscanf(line, "%lf %lf %lf %lf %lf %d", &a, &b, &c, &d, &cost, &studySettled) != 6) {
                continue;
            }
            const RouteResult outcome = search.find(source, a, b, c, d, profile, scratch, &stats);
            logInfo("Bench %s %u: %s, %.0f m (study %.0f), settled %u (study %d), tiles %u, %u ms (reading %u), peak %u KB, pass %u", routeProfileName(profile),
                static_cast<unsigned>(count), routeResultText(outcome), scratch.lengthM, cost, stats.settled, studySettled, stats.tilesRead, stats.totalMs,
                stats.loadMs, stats.peakBytes / 1024, static_cast<unsigned>(stats.passes));
            count++;
            found += outcome == RouteResult::Found ? 1 : 0;
            tooLong += outcome == RouteResult::TooLong ? 1 : 0;
            ms += stats.totalMs;
            maxMs = std::max(maxMs, stats.totalMs);
            settled += stats.settled;
            maxSettled = std::max(maxSettled, stats.settled);
            tiles += stats.tilesRead;
            maxTiles = std::max(maxTiles, stats.tilesRead);
            peak = std::max(peak, stats.peakBytes);
            scratch.clear();
        }
        if (count > 0) {
            logInfo("Bench %s: %u of %u found (%u too long); %u ms mean, %u max; settled %u mean, %u max; tiles %.1f mean, %u max; peak %u KB",
                routeProfileName(profile), found, count, tooLong, static_cast<unsigned>(ms / count), maxMs, static_cast<unsigned>(settled / count), maxSettled,
                static_cast<double>(tiles) / count, maxTiles, peak / 1024);
        }
    }
    giveMemory();
    result = RouteResult::Found;
    currentState = State::Done;
}

bool Navigator::saveRoute(const Route& route) {
    GrowArray<uint8_t> out;
    for (char c : ROUTE_MAGIC) {
        out.push(static_cast<uint8_t>(c));
    }
    putU16(out, ROUTE_FILE_VERSION);
    // The top bit: a route made from a GPX track
    putU16(out, static_cast<uint16_t>(static_cast<uint16_t>(route.profile) | (route.track ? 0x8000u : 0u)));
    putU32(out, static_cast<uint32_t>(route.from.x));
    putU32(out, static_cast<uint32_t>(route.from.y));
    putU32(out, static_cast<uint32_t>(route.to.x));
    putU32(out, static_cast<uint32_t>(route.to.y));
    putU32(out, static_cast<uint32_t>(route.seconds));
    putU32(out, static_cast<uint32_t>(route.points.size()));
    putU32(out, static_cast<uint32_t>(route.steps.size()));
    putU32(out, static_cast<uint32_t>(route.names.size()));
    for (const RoutePoint& point : route.points) {
        putU32(out, static_cast<uint32_t>(point.x));
        putU32(out, static_cast<uint32_t>(point.y));
    }
    for (const RouteStep& step : route.steps) {
        putU32(out, step.point);
        putU16(out, step.nameOffset);
        out.push(step.nameLength);
        out.push(step.roadClass);
        out.push(step.flags);
    }
    for (char c : route.names) {
        out.push(static_cast<uint8_t>(c));
    }
    if (out.failed()) {
        return false;
    }
    char temporary[PATH_MAX_LENGTH + 8];
    snprintf(temporary, sizeof(temporary), "%s.tmp", routePath);
    auto write = [&]() { return io->writeFile(routePath, temporary, out.begin(), out.size(), false); };
    return runOnCard(*io, write);
}

bool Navigator::loadRoute() {
    FileBuffer file;
    auto read = [&]() { return io->readFile(routePath, file); };
    if (!runOnCard(*io, read) || file.size() < 44 || memcmp(file.bytes(), ROUTE_MAGIC, 4) != 0) {
        return false;
    }
    const uint8_t* data = file.bytes();
    if ((data[4] | (data[5] << 8)) != ROUTE_FILE_VERSION) {
        return false;
    }
    Route& route = routes[current];
    route.clear();
    route.profile = static_cast<RouteProfile>(std::min<int>(data[6], static_cast<int>(RouteProfile::Car)));
    route.track = (data[7] & 0x80) != 0;
    route.from = { static_cast<int32_t>(readU32(data + 8)), static_cast<int32_t>(readU32(data + 12)) };
    route.to = { static_cast<int32_t>(readU32(data + 16)), static_cast<int32_t>(readU32(data + 20)) };
    route.seconds = static_cast<float>(readU32(data + 24));
    const uint32_t points = readU32(data + 28);
    const uint32_t steps = readU32(data + 32);
    const uint32_t names = readU32(data + 36);
    const size_t expected = 40 + static_cast<size_t>(points) * 8 + static_cast<size_t>(steps) * 9 + names;
    if (expected != file.size() || points < 2) {
        return false;
    }
    const uint8_t* cursor = data + 40;
    for (uint32_t i = 0; i < points; i++, cursor += 8) {
        route.points.push({ static_cast<int32_t>(readU32(cursor)), static_cast<int32_t>(readU32(cursor + 4)) });
    }
    for (uint32_t i = 0; i < steps; i++, cursor += 9) {
        route.steps.push({ readU32(cursor), static_cast<uint16_t>(cursor[4] | (cursor[5] << 8)), cursor[6], cursor[7], cursor[8] });
    }
    for (uint32_t i = 0; i < names; i++) {
        route.names.push(static_cast<char>(cursor[i]));
    }
    if (!route.distances.resize(points)) {
        route.clear();
        return false;
    }
    float distance = 0.0f;
    for (uint32_t i = 1; i < points; i++) {
        const RoutePoint& a = route.points[i - 1];
        const RoutePoint& b = route.points[i];
        const float dx = static_cast<float>(b.x - a.x);
        const float dy = static_cast<float>(b.y - a.y);
        distance += std::sqrt(dx * dx + dy * dy) * static_cast<float>(routeMetresPerUnit((a.y + b.y) / 2));
        route.distances[i] = distance;
    }
    route.lengthM = distance;
    routeReady = !route.points.failed();
    changes++;
    return routeReady;
}

} // namespace maps
