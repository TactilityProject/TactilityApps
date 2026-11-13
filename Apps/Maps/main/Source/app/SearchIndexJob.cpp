// SPDX-License-Identifier: GPL-3.0-or-later
#include "SearchIndexJob.h"

#include "../mvt/MvtDecoder.h"
#include "../platform/Platform.h"
#include "../route/GrowArray.h"
#include "../search/SearchStream.h"
#include "../tiles/RegionMeta.h"
#include "../tiles/TileStore.h"

#include <esp_heap_caps.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace maps {

namespace {

constexpr uint32_t TASK_STACK = 24 * 1024;
/** Below the render lanes and the I/O task: the build gets the CPU when the map has nothing to do. */
constexpr UBaseType_t TASK_PRIORITY = 1;
/** Tiles per side of an index block. */
constexpr int32_t BLOCK_TILES = 15;
/** A tile is read in pieces of this size, each piece one I/O job that opens and closes the file. */
constexpr uint32_t TILE_CHUNK = 16 * 1024;
/** The sort buffers on the device (two at most at once). */
constexpr uint32_t SORT_BUFFER = 64 * 1024;
/** Files the build keeps open at once: the card's FAT mount allows four for the whole system. */
constexpr int REAL_FILES = 2;
/** The stamp's format: change it to rebuild every index (e.g. after a change of the builder's output). */
constexpr char STAMP_VERSION[] = "sidx1";

template <typename Function>
bool onCard(IoTask& io, Function& function) {
    return runOnCard(io, function, IoPriority::Background);
}

/**
 * ScratchFiles on the card through the I/O task. Handles are virtual: at most REAL_FILES are open as FILE*, the others
 * are closed and opened again at their position when used (written files are only appended to).
 */
class CardScratchFiles final : public ScratchFiles {
public:
    explicit CardScratchFiles(IoTask& io) : io(io) {}
    ~CardScratchFiles() override {
        for (int i = 0; i < MAX_OPEN; i++) {
            if (handles[i].used) {
                close(i);
            }
        }
    }

    int create(const char* path) override { return add(path, true, true); }
    int openRead(const char* path) override { return add(path, false, false); }
    int openAppend(const char* path) override { return add(path, true, false); }

    bool write(int h, const void* data, uint32_t size) override {
        Handle* handle = at(h);
        if (handle == nullptr || !handle->writing) {
            return false;
        }
        bool ok = false;
        auto job = [&]() {
            ok = ensure(*handle) && (size == 0 || fwrite(data, 1, size, handle->file) == size);
            return ok;
        };
        onCard(io, job);
        if (ok) {
            handle->position += size;
        }
        return ok;
    }

    int read(int h, void* data, uint32_t size) override {
        Handle* handle = at(h);
        if (handle == nullptr || handle->writing) {
            return -1;
        }
        int result = -1;
        auto job = [&]() {
            if (!ensure(*handle)) {
                return false;
            }
            const size_t n = fread(data, 1, size, handle->file);
            result = n == 0 && ferror(handle->file) ? -1 : static_cast<int>(n);
            return result >= 0;
        };
        onCard(io, job);
        if (result > 0) {
            handle->position += static_cast<uint32_t>(result);
        }
        return result;
    }

    bool seek(int h, uint32_t offset) override {
        Handle* handle = at(h);
        if (handle == nullptr || handle->writing) {
            return false;
        }
        handle->position = offset;
        if (handle->file == nullptr) {
            return true;   // positioned when it is opened again
        }
        bool ok = false;
        auto job = [&]() {
            ok = fseek(handle->file, static_cast<long>(offset), SEEK_SET) == 0;
            return ok;
        };
        onCard(io, job);
        return ok;
    }

    bool close(int h) override {
        Handle* handle = at(h);
        if (handle == nullptr) {
            return false;
        }
        bool ok = true;
        if (handle->file != nullptr) {
            auto job = [&]() {
                ok = fclose(handle->file) == 0;
                return ok;
            };
            onCard(io, job);
            handle->file = nullptr;
            realOpen--;
        }
        handle->used = false;
        return ok;
    }

    bool remove(const char* path) override {
        bool gone = false;
        auto job = [&]() {
            ::remove(path);
            gone = !fileExists(path);
            return gone;
        };
        onCard(io, job);
        return gone;
    }

    bool rename(const char* from, const char* to) override {
        bool ok = false;
        auto job = [&]() {
            ::remove(to);
            ok = ::rename(from, to) == 0;
            return ok;
        };
        onCard(io, job);
        return ok;
    }

    /** Closes every real file (the handles stay valid): while the build waits, the card's few files are free. */
    void suspend() {
        for (Handle& handle : handles) {
            if (handle.used && handle.file != nullptr) {
                auto job = [&]() {
                    fclose(handle.file);
                    return true;
                };
                onCard(io, job);
                handle.file = nullptr;
                realOpen--;
            }
        }
    }

private:
    struct Handle {
        bool used;
        bool writing;
        bool truncate;
        char path[PATH_MAX_LENGTH];
        uint32_t position;
        uint32_t lastUse;
        FILE* file;
    };

    int add(const char* path, bool writing, bool truncate) {
        for (int i = 0; i < MAX_OPEN; i++) {
            if (!handles[i].used) {
                Handle& handle = handles[i];
                handle = {};
                handle.used = true;
                handle.writing = writing;
                handle.truncate = truncate;
                snprintf(handle.path, sizeof(handle.path), "%s", path);
                // Opened at once, so a missing file fails here
                bool ok = false;
                auto job = [&]() {
                    ok = ensure(handle);
                    return ok;
                };
                onCard(io, job);
                if (!ok) {
                    handle.used = false;
                    return -1;
                }
                return i;
            }
        }
        return -1;
    }

    Handle* at(int h) { return h >= 0 && h < MAX_OPEN && handles[h].used ? &handles[h] : nullptr; }

    /** On the I/O task: the handle's FILE*, opening it (and closing the least recently used one) when needed. */
    bool ensure(Handle& handle) {
        handle.lastUse = ++useCounter;
        if (handle.file != nullptr) {
            return true;
        }
        if (realOpen >= REAL_FILES) {
            Handle* oldest = nullptr;
            for (Handle& other : handles) {
                if (other.used && other.file != nullptr && (oldest == nullptr || other.lastUse < oldest->lastUse)) {
                    oldest = &other;
                }
            }
            if (oldest != nullptr) {
                fclose(oldest->file);
                oldest->file = nullptr;
                realOpen--;
            }
        }
        if (handle.writing) {
            handle.file = fopen(handle.path, handle.truncate ? "wb" : "ab");
            handle.truncate = false;
        } else {
            handle.file = fopen(handle.path, "rb");
            if (handle.file != nullptr && handle.position > 0 && fseek(handle.file, static_cast<long>(handle.position), SEEK_SET) != 0) {
                fclose(handle.file);
                handle.file = nullptr;
            }
        }
        if (handle.file == nullptr) {
            return false;
        }
        realOpen++;
        return true;
    }

    IoTask& io;
    Handle handles[MAX_OPEN] = {};
    int realOpen = 0;
    uint32_t useCounter = 0;
};

/** A tile's .pbf read in pieces through the I/O task (each piece opens and closes the file). */
class CardTileInput final : public ByteInput {
public:
    CardTileInput(IoTask& io, uint8_t* buffer) : io(io), buffer(buffer) {}
    void reset(const char* tilePath) {
        snprintf(path, sizeof(path), "%s", tilePath);
        offset = 0;
        filled = 0;
        at = 0;
        failed = false;
    }
    int read(uint8_t* out, size_t size) override {
        size_t copied = 0;
        while (copied < size) {
            if (at == filled) {
                if (!refill()) {
                    return failed ? -1 : static_cast<int>(copied);
                }
            }
            const size_t n = std::min(size - copied, static_cast<size_t>(filled - at));
            memcpy(out + copied, buffer + at, n);
            at += static_cast<uint32_t>(n);
            copied += n;
        }
        return static_cast<int>(copied);
    }
    bool rewind() override {
        offset = 0;
        filled = 0;
        at = 0;
        return true;
    }

private:
    bool refill() {
        uint32_t got = 0;
        bool ok = false;
        auto job = [&]() {
            FILE* file = fopen(path, "rb");
            if (file == nullptr) {
                return false;
            }
            ok = fseek(file, static_cast<long>(offset), SEEK_SET) == 0;
            if (ok) {
                got = static_cast<uint32_t>(fread(buffer, 1, TILE_CHUNK, file));
            }
            fclose(file);
            return ok;
        };
        onCard(io, job);
        if (!ok) {
            failed = true;
            return false;
        }
        offset += got;
        filled = got;
        at = 0;
        return got > 0;
    }

    IoTask& io;
    uint8_t* buffer;
    char path[PATH_MAX_LENGTH] = "";
    uint32_t offset = 0;
    uint32_t filled = 0;
    uint32_t at = 0;
    bool failed = false;
};

class CardTileSource final : public StreamTileSource {
public:
    CardTileSource(IoTask& io, const char* regionPath, uint8_t* buffer, MvtDecoder*& decoder) : input(io, buffer), decoder(decoder) {
        snprintf(region, sizeof(region), "%s", regionPath);
    }
    ByteInput* open(int32_t x, int32_t y) override {
        char path[PATH_MAX_LENGTH];
        snprintf(path, sizeof(path), "%s/14/%d/%d.pbf", region, static_cast<int>(x), static_cast<int>(y));
        input.reset(path);
        return &input;
    }
    /** The decoder's pools are freed here (its memory block is the job's). */
    void tilesDone() override {
        if (decoder != nullptr) {
            decoder->~MvtDecoder();
            decoder = nullptr;
        }
    }

private:
    char region[PATH_MAX_LENGTH] = "";
    CardTileInput input;
    MvtDecoder*& decoder;
};

struct TileList {
    GrowArray<StreamTile> tiles;
    char path[PATH_MAX_LENGTH];
    int32_t x = 0;
};

bool collectTiles(const char* name, bool isDirectory, void* context) {
    auto* list = static_cast<TileList*>(context);
    if (isDirectory) {
        return true;
    }
    const size_t length = strlen(name);
    if (length < 5 || strcmp(name + length - 4, ".pbf") != 0) {
        return true;
    }
    char* end = nullptr;
    const long y = strtol(name, &end, 10);
    if (end == name || strcmp(end, ".pbf") != 0) {
        return true;
    }
    list->tiles.push({ list->x, static_cast<int32_t>(y) });
    return true;
}

struct ColumnList {
    GrowArray<int32_t> columns;
};

bool collectColumns(const char* name, bool isDirectory, void* context) {
    if (!isDirectory) {
        return true;
    }
    char* end = nullptr;
    const long x = strtol(name, &end, 10);
    if (end != name && *end == '\0') {
        static_cast<ColumnList*>(context)->columns.push(static_cast<int32_t>(x));
    }
    return true;
}

struct RegionList {
    char names[TileStore::MAX_REGIONS][16];
    int count = 0;
};

bool collectRegions(const char* name, bool isDirectory, void* context) {
    auto* list = static_cast<RegionList*>(context);
    if (isDirectory && strlen(name) < sizeof(list->names[0]) && strcmp(name, BROWSE_REGION) != 0 && list->count < TileStore::MAX_REGIONS) {
        snprintf(list->names[list->count++], sizeof(list->names[0]), "%s", name);
    }
    return true;
}

struct FileName {
    char text[16];
};

/** Names of index files (in PSRAM: a country has many blocks). */
struct IndexFiles {
    GrowArray<FileName> names;
    void add(const char* name) {
        FileName entry = {};
        snprintf(entry.text, sizeof(entry.text), "%s", name);
        names.push(entry);
    }
    bool has(const char* name) const {
        for (const FileName& entry : names) {
            if (strcmp(entry.text, name) == 0) {
                return true;
            }
        }
        return false;
    }
};

bool collectIndexFiles(const char* name, bool isDirectory, void* context) {
    auto* list = static_cast<IndexFiles*>(context);
    const size_t length = strlen(name);
    if (!isDirectory && length > 4 && strcmp(name + length - 4, ".idx") == 0 && length < sizeof(FileName::text)) {
        list->add(name);
    }
    return true;
}

/** What the builder's hook needs: the job, and the build's files (closed while the build waits for a download). */
struct HookContext {
    SearchIndexJob* job;
    CardScratchFiles* files;
};

bool keepGoingHook(const StreamProgress& progress, void* context) {
    auto* hook = static_cast<HookContext*>(context);
    if (hook->job->isPaused()) {
        // The FAT mount allows 4 open files for the whole system: the download needs them
        hook->files->suspend();
    }
    return hook->job->keepGoing(static_cast<int32_t>(progress.done), static_cast<int32_t>(progress.total));
}

/** The task suspends itself when its work is over: it must not run any more when it is deleted. */
void waitUntilSuspended(TaskHandle_t task) {
    TaskStatus_t status {};
    for (;;) {
        vTaskGetInfo(task, &status, pdFALSE, eInvalid);
        if (status.eCurrentState == eSuspended) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

} // namespace

SearchIndexJob::~SearchIndexJob() {
    stop();
}

bool SearchIndexJob::start(IoTask& ioTask, const char* rootPath, const char* first, bool force, const char* only) {
    if (task != nullptr || currentState.load() == State::Busy) {
        return false;
    }
    io = &ioTask;
    snprintf(root, sizeof(root), "%s", rootPath);
    snprintf(firstRegion, sizeof(firstRegion), "%s", first != nullptr ? first : "");
    snprintf(onlyRegion, sizeof(onlyRegion), "%s", only != nullptr ? only : "");
    forceBuild = force;
    written = 0;
    stopping = false;
    paused = false;
    progressDone = 0;
    progressTotal = 0;
    lowestFree = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    currentState = State::Busy;
    if (xTaskCreatePinnedToCoreWithCaps(taskMain, "maps-sidx", TASK_STACK, this, TASK_PRIORITY, &task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM) !=
        pdPASS) {
        task = nullptr;
        currentState = State::Failed;
        return false;
    }
    return true;
}

void SearchIndexJob::stop() {
    if (task == nullptr) {
        return;
    }
    stopping = true;
    paused = false;
    while (currentState.load() == State::Busy) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // The state is set just before the task suspends itself; delete it from here once it has
    waitUntilSuspended(task);
    vTaskDeleteWithCaps(task);
    task = nullptr;
}

void SearchIndexJob::acknowledge() {
    if (currentState.load() != State::Busy) {
        if (task != nullptr) {
            waitUntilSuspended(task);
            vTaskDeleteWithCaps(task);
            task = nullptr;
        }
        currentState = State::Idle;
    }
}

void SearchIndexJob::taskMain(void* context) {
    auto* self = static_cast<SearchIndexJob*>(context);
    self->run();
    vTaskSuspend(nullptr);
}

bool SearchIndexJob::keepGoing(int32_t done, int32_t total) {
    progressDone = done;
    progressTotal = total;
    const auto free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    lowestFree = std::min(lowestFree, free);
    while (paused.load() && !stopping.load()) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    return !stopping.load();
}

void SearchIndexJob::report(const char* format, ...) {
    const uint32_t index = reportWritten.load();
    if (index - reportRead.load() >= REPORT_LINES) {
        return;   // the map loop did not read the older lines yet: drop this one
    }
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(reports[index % REPORT_LINES], sizeof(reports[0]), format, arguments);
    va_end(arguments);
    logInfo("%s", reports[index % REPORT_LINES]);
    reportWritten = index + 1;
}

const char* SearchIndexJob::nextReport() {
    const uint32_t index = reportRead.load();
    if (index == reportWritten.load()) {
        return nullptr;
    }
    snprintf(reportOut, sizeof(reportOut), "%s", reports[index % REPORT_LINES]);
    reportRead = index + 1;
    return reportOut;
}

void SearchIndexJob::run() {
    RegionList regions;
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/maps", root);
    auto list = [&]() { return listDirectory(path, collectRegions, &regions); };
    onCard(*io, list);
    // The map's region first
    for (int i = 0; i < regions.count; i++) {
        if (strcmp(regions.names[i], firstRegion) == 0 && i > 0) {
            char swap[16];
            memcpy(swap, regions.names[0], sizeof(swap));
            memcpy(regions.names[0], regions.names[i], sizeof(swap));
            memcpy(regions.names[i], swap, sizeof(swap));
        }
    }
    bool ok = true;
    for (int i = 0; i < regions.count && !stopping.load(); i++) {
        if (onlyRegion[0] != '\0' && strcmp(regions.names[i], onlyRegion) != 0) {
            continue;
        }
        ok = buildRegion(regions.names[i]) && ok;
    }
    currentState = ok ? State::Done : State::Failed;
}

bool SearchIndexJob::buildRegion(const char* region) {
    char regionPath[PATH_MAX_LENGTH];
    snprintf(regionPath, sizeof(regionPath), "%s/maps/%s", root, region);
    // The stamp: what the index was built from
    char stamp[200] = "";
    {
        char metaPath[PATH_MAX_LENGTH];
        snprintf(metaPath, sizeof(metaPath), "%s/meta.json", regionPath);
        RegionMeta meta = {};
        bool haveMeta = false;
        auto readMeta = [&]() {
            haveMeta = readRegionMeta(metaPath, meta);
            return true;
        };
        onCard(*io, readMeta);
        if (haveMeta && meta.total > 0 && !meta.isComplete()) {
            return true;   // still downloading: built when it is complete
        }
        snprintf(stamp, sizeof(stamp), "%s %s %s %lld %llu", STAMP_VERSION, haveMeta ? meta.version : "-", haveMeta ? meta.date : "-",
                 haveMeta ? static_cast<long long>(meta.done) : -1LL, haveMeta ? static_cast<unsigned long long>(meta.bytes) : 0ULL);
    }
    char stampPath[PATH_MAX_LENGTH];
    snprintf(stampPath, sizeof(stampPath), "%s/search.inf", regionPath);
    if (!forceBuild) {
        char old[200] = "";
        auto readStamp = [&]() {
            FILE* file = fopen(stampPath, "rb");
            if (file == nullptr) {
                return false;
            }
            const size_t n = fread(old, 1, sizeof(old) - 1, file);
            old[n] = '\0';
            fclose(file);
            return true;
        };
        onCard(*io, readStamp);
        char* newline = strchr(old, '\n');
        if (newline != nullptr) {
            *newline = '\0';
        }
        if (strcmp(old, stamp) == 0) {
            return true;   // current
        }
    }
    // The z14 tiles, sorted by x, then y
    TileList list;
    {
        ColumnList columns;
        snprintf(list.path, sizeof(list.path), "%s/14", regionPath);
        auto walkColumns = [&]() { return listDirectory(list.path, collectColumns, &columns); };
        onCard(*io, walkColumns);
        for (int32_t x : columns.columns) {
            char columnPath[PATH_MAX_LENGTH];
            snprintf(columnPath, sizeof(columnPath), "%s/14/%d", regionPath, static_cast<int>(x));
            list.x = x;
            auto walk = [&]() { return listDirectory(columnPath, collectTiles, &list); };
            onCard(*io, walk);
        }
    }
    if (list.tiles.size() == 0 || list.tiles.failed()) {
        return true;   // nothing to index
    }
    std::sort(list.tiles.begin(), list.tiles.end(), [](const StreamTile& a, const StreamTile& b) { return a.x != b.x ? a.x < b.x : a.y < b.y; });
    int32_t minX = list.tiles[0].x, maxX = minX, minY = list.tiles[0].y, maxY = minY;
    for (const StreamTile& t : list.tiles) {
        minX = std::min(minX, t.x);
        maxX = std::max(maxX, t.x);
        minY = std::min(minY, t.y);
        maxY = std::max(maxY, t.y);
    }
    const bool blocks = maxX - minX >= BLOCK_TILES || maxY - minY >= BLOCK_TILES;
    char scratch[PATH_MAX_LENGTH];
    snprintf(scratch, sizeof(scratch), "%s/srch", regionPath);
    char blockFolder[PATH_MAX_LENGTH];
    snprintf(blockFolder, sizeof(blockFolder), "%s/search", regionPath);
    auto makeFolders = [&]() {
        mkdir(scratch, 0777);
        if (blocks) {
            mkdir(blockFolder, 0777);
        }
        return true;
    };
    onCard(*io, makeFolders);
    // Memory for the build: decoder, a tile buffer, the builder (freed at the end)
    void* decoderMemory = largeAlloc(sizeof(MvtDecoder));
    auto* buffer = static_cast<uint8_t*>(largeAlloc(TILE_CHUNK));
    void* builderMemory = largeAlloc(sizeof(SearchStreamBuilder));
    if (decoderMemory == nullptr || buffer == nullptr || builderMemory == nullptr) {
        largeFree(decoderMemory);
        largeFree(buffer);
        largeFree(builderMemory);
        report("Search index %s: no memory to start", region);
        return false;
    }
    auto* builder = new (builderMemory) SearchStreamBuilder();
    CardScratchFiles files(*io);
    HookContext hook { this, &files };
    StreamBuildOptions options;
    options.sortBufferBytes = SORT_BUFFER;
    options.keepGoing = keepGoingHook;
    options.context = &hook;
    const char* profile = strcmp(region, "kyiv") == 0 ? "uk" : (strcmp(region, "belgrade") == 0 ? "sr" : "ru");
    options.config.profile = profile[0] == 'u' ? Translit::Ukrainian : (profile[0] == 's' ? Translit::Serbian : Translit::Russian);
    // One block at a time (all tiles when the region is small)
    GrowArray<StreamTile> blockTiles;
    IndexFiles made;
    bool ok = true;
    const uint32_t regionStarted = nowMs();
    uint64_t indexBytes = 0;
    for (int32_t by = 0; ok && by <= (blocks ? (maxY - minY) / BLOCK_TILES : 0); by++) {
        for (int32_t bx = 0; ok && bx <= (blocks ? (maxX - minX) / BLOCK_TILES : 0); bx++) {
            blockTiles.clear();
            for (const StreamTile& t : list.tiles) {
                if (!blocks || ((t.x - minX) / BLOCK_TILES == bx && (t.y - minY) / BLOCK_TILES == by)) {
                    blockTiles.push(t);
                }
            }
            if (blockTiles.size() == 0) {
                continue;
            }
            char outPath[PATH_MAX_LENGTH];
            char temporary[PATH_MAX_LENGTH];
            char blockName[16] = "";
            if (blocks) {
                snprintf(blockName, sizeof(blockName), "%d_%d.idx", static_cast<int>(bx), static_cast<int>(by));
                made.add(blockName);
                snprintf(outPath, sizeof(outPath), "%s/%s", blockFolder, blockName);
                snprintf(temporary, sizeof(temporary), "%s/%d_%d.tmp", blockFolder, static_cast<int>(bx), static_cast<int>(by));
            } else {
                snprintf(outPath, sizeof(outPath), "%s/search.idx", regionPath);
                snprintf(temporary, sizeof(temporary), "%s/search.tmp", regionPath);
            }
            auto* decoder = new (decoderMemory) MvtDecoder();
            if (!decoder->init()) {
                decoder->~MvtDecoder();
                ok = false;
                report("Search index %s: no memory for the decoder", region);
                break;
            }
            MvtDecoder* owned = decoder;
            CardTileSource source(*io, regionPath, buffer, owned);
            const uint32_t freeBefore = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            lowestFree = freeBefore;
            const uint32_t started = nowMs();
            ok = builder->build(files, scratch, blockTiles.begin(), static_cast<uint32_t>(blockTiles.size()), source, *decoder, outPath, temporary,
                                options);
            files.suspend();
            if (owned != nullptr) {
                owned->~MvtDecoder();
            }
            const StreamBuildStats& s = builder->stats();
            const RegionStats& r = s.region;
            if (!ok) {
                report("Search index %s: %s", region, builder->cancelled() ? "stopped" : builder->error());
                break;
            }
            indexBytes += s.index.totalBytes;
            written++;
            report("Search index %s%s%s: %u tiles in %u ms, %u bytes; PSRAM %u KB free before, %u KB lowest", region, blocks ? " " : "",
                   blockName, static_cast<unsigned>(blockTiles.size()), static_cast<unsigned>(nowMs() - started),
                   static_cast<unsigned>(s.index.totalBytes), static_cast<unsigned>(freeBefore / 1024), static_cast<unsigned>(lowestFree / 1024));
            report("Search index phases ms: tiles %u points %u clusters %u matching %u addresses %u groups %u keys %u file %u", s.phaseMs[0],
                   s.phaseMs[1], s.phaseMs[2], s.phaseMs[3], s.phaseMs[4], s.phaseMs[5], s.phaseMs[6], s.phaseMs[7]);
            report("Search index slowest tile %d/%d %u ms, matching %d/%d %u ms; scratch %u KB, sort %u KB, tables %u KB, matcher %u KB",
                   static_cast<int>(s.slowestExtractTile.x), static_cast<int>(s.slowestExtractTile.y), s.slowestExtractMs,
                   static_cast<int>(s.slowestMatchTile.x), static_cast<int>(s.slowestMatchTile.y), s.slowestMatchMs,
                   static_cast<unsigned>(s.scratchBytes / 1024), static_cast<unsigned>(s.sortPeakBytes / 1024), static_cast<unsigned>(s.tableBytes / 1024),
                   static_cast<unsigned>(s.matcherPeakBytes / 1024));
            report("Search index counts: places %u named POIs %u streets %u clusters %u houses %u (plain %.1f %%, village mode %d); street50 %u "
                   "street100 %u village %u none %u, %u village streets",
                   r.places, r.namedPois, r.streetNames, r.clusters, r.houses, 100.0 * r.plainShare, r.villageMode ? 1 : 0, r.pathStreet50,
                   r.pathStreet100, r.pathVillage, r.pathNone, r.pseudoStreets);
        }
    }
    builder->~SearchStreamBuilder();
    largeFree(builderMemory);
    largeFree(buffer);
    largeFree(decoderMemory);
    // The scratch folder, files of an older layout (one file vs blocks), then the stamp
    auto cleanup = [&]() {
        removeFolder(scratch);
        if (!ok) {
            return true;
        }
        char other[PATH_MAX_LENGTH];
        if (blocks) {
            snprintf(other, sizeof(other), "%s/search.idx", regionPath);
            ::remove(other);
            IndexFiles existing;
            listDirectory(blockFolder, collectIndexFiles, &existing);
            for (const FileName& entry : existing.names) {
                if (!made.has(entry.text)) {
                    snprintf(other, sizeof(other), "%s/%s", blockFolder, entry.text);
                    ::remove(other);
                }
            }
        } else {
            IndexFiles existing;
            if (listDirectory(blockFolder, collectIndexFiles, &existing)) {
                for (const FileName& entry : existing.names) {
                    snprintf(other, sizeof(other), "%s/%s", blockFolder, entry.text);
                    ::remove(other);
                }
                removeFolder(blockFolder);
            }
        }
        FILE* file = fopen(stampPath, "wb");
        if (file != nullptr) {
            fprintf(file, "%s\n", stamp);
            fclose(file);
        }
        return true;
    };
    onCard(*io, cleanup);
    if (ok) {
        report("Search index %s ready: %u blocks, %u bytes in %u ms", region, static_cast<unsigned>(blocks ? made.names.size() : 1),
               static_cast<unsigned>(indexBytes), static_cast<unsigned>(nowMs() - regionStarted));
    }
    return ok;
}

} // namespace maps
