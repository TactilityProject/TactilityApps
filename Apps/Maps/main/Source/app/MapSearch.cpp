// SPDX-License-Identifier: GPL-3.0-or-later
// Offline search on the map (M6): the search sheet, the region indexes on the card, opening a result.
#include "MapView.h"

#include "../platform/Platform.h"
#include "../route/RouteSearch.h"

#include <esp_heap_caps.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <new>

namespace maps {

namespace {

/** Results fetched at most (the first page shows at once, the rest while scrolling). */
constexpr int SEARCH_MAX_RESULTS = SearchSheet::MAX_ROWS;
constexpr char RECENT_FILE[] = "recent.txt";

struct RangeReadJob {
    FILE** file;
    const char* path;
    uint32_t offset;
    void* out;
    uint32_t size;
};

bool rangeReadJob(IoTask& io, void* context) {
    (void)io;
    auto* job = static_cast<RangeReadJob*>(context);
    if (*job->file == nullptr) {
        *job->file = fopen(job->path, "rb");
        if (*job->file == nullptr) {
            return false;
        }
    }
    if (fseek(*job->file, static_cast<long>(job->offset), SEEK_SET) != 0) {
        return false;
    }
    return fread(job->out, 1, job->size, *job->file) == job->size;
}

bool closeFileJob(IoTask& io, void* context) {
    (void)io;
    auto** file = static_cast<FILE**>(context);
    if (*file != nullptr) {
        fclose(*file);
        *file = nullptr;
    }
    return true;
}

void searchCoordinates(double lat, double lon, char* out, size_t size) {
    snprintf(out, size, "%.5f %c  %.5f %c", std::fabs(lat), lat >= 0.0 ? 'N' : 'S', std::fabs(lon), lon >= 0.0 ? 'E' : 'W');
}

/** Runs a callable on the search task (its stack is large enough for the engine), waiting for it. */
template <typename Function>
void onSearchTask(SearchTask& task, Function& function) {
    task.run([](void* context) { (*static_cast<Function*>(context))(); }, &function);
}

uint64_t searchClockUs() {
    return nowUs();
}

const char* plainDistance(uint32_t metres, char* out, size_t size) {
    formatDistance(static_cast<float>(metres), out, size);
    return out;
}

} // namespace

/**
 * A region's index file read through the I/O task. The file stays open between reads, but only one index file is
 * open at a time: the card's FAT mount allows four open files for the whole system (the index build keeps two).
 */
class MapView::CardSearchFile final : public SearchFile {
public:
    CardSearchFile(IoTask& io, const char* path) : io(io) { snprintf(this->path, sizeof(this->path), "%s", path); }
    ~CardSearchFile() override {
        close();
        if (openOne == this) {
            openOne = nullptr;
        }
    }
    bool read(uint32_t offset, void* out, uint32_t size) override {
        if (openOne != this) {
            if (openOne != nullptr) {
                openOne->close();
            }
            openOne = this;
        }
        RangeReadJob job { &file, path, offset, out, size };
        reads++;
        return io.run(IoPriority::Visible, rangeReadJob, &job);
    }
    void close() {
        if (file != nullptr) {
            io.run(IoPriority::Visible, closeFileJob, &file);
        }
    }
    uint32_t reads = 0;

private:
    static CardSearchFile* openOne;
    IoTask& io;
    char path[PATH_MAX_LENGTH] {};
    FILE* file = nullptr;
};

MapView::CardSearchFile* MapView::CardSearchFile::openOne = nullptr;

namespace {

/** Block files of a big region (the .idx files in maps/<region>/search). */
struct BlockFiles {
    char names[64][16];
    int count = 0;
};

bool collectBlocks(const char* name, bool isDirectory, void* context) {
    auto* list = static_cast<BlockFiles*>(context);
    const size_t length = strlen(name);
    if (!isDirectory && length > 4 && length < sizeof(list->names[0]) && strcmp(name + length - 4, ".idx") == 0 && list->count < 64) {
        snprintf(list->names[list->count++], sizeof(list->names[0]), "%s", name);
    }
    return true;
}

} // namespace

bool MapView::loadSearchIndexes() {
    if (searchEngine == nullptr) {
        void* memory = largeAlloc(sizeof(SearchEngine));
        void* results = largeAlloc(sizeof(SearchResult) * SEARCH_MAX_RESULTS);
        void* recent = largeAlloc(sizeof(RecentSearches));
        if (memory == nullptr || results == nullptr || recent == nullptr) {
            largeFree(memory);
            largeFree(results);
            largeFree(recent);
            return false;
        }
        searchEngine = new (memory) SearchEngine();
        searchEngine->setClock(searchClockUs);
        searchResults = static_cast<SearchResult*>(results);
        recentSearches = new (recent) RecentSearches();
        loadRecentSearches();
    }
    // The indexes of the regions on the card, the one under the map centre first
    for (int i = 0; i < searchFileCount; i++) {
        searchFiles[i]->~CardSearchFile();
        largeFree(searchFiles[i]);
        searchFiles[i] = nullptr;
    }
    searchFileCount = 0;
    auto removeAll = [&]() { searchEngine->removeAll(); };
    onSearchTask(searchTask, removeAll);
    const double lat = worldYToLat(centerY, zoom);
    const double lon = worldXToLon(centerX, zoom);
    const TileKey centre = tileAt(lat, lon, 14);
    int order[TileStore::MAX_REGIONS];
    int orderCount = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < store.regionCount(); i++) {
            const bool here = store.mayHave(i, centre);
            if ((pass == 0) == here) {
                order[orderCount++] = i;
            }
        }
    }
    auto addFile = [&](const char* path, const char* region) {
        void* memory = largeAlloc(sizeof(CardSearchFile));
        if (memory == nullptr) {
            return;
        }
        auto* file = new (memory) CardSearchFile(io, path);
        // The profile and the x/y rule come from the index header (the engine reads it)
        int slot = -1;
        auto add = [&]() { slot = searchEngine->addIndex(file, region, Translit::Russian, true); };
        onSearchTask(searchTask, add);
        if (slot < 0) {
            logError("Search: %s does not open", path);
            file->~CardSearchFile();
            largeFree(memory);
            return;
        }
        searchFiles[searchFileCount++] = file;
    };
    for (int k = 0; k < orderCount && searchFileCount < SEARCH_MAX_INDEXES; k++) {
        const char* region = store.regionName(order[k]);
        char path[PATH_MAX_LENGTH];
        snprintf(path, sizeof(path), "%s/maps/%s/search.idx", rootPath, region);
        bool exists = false;
        auto blocks = new BlockFiles();
        char folder[PATH_MAX_LENGTH];
        snprintf(folder, sizeof(folder), "%s/maps/%s/search", rootPath, region);
        // A big region has blocks instead: their origins (bytes 8..15 of the header) say which are nearest the centre
        int64_t distance[64] = {};
        auto check = [&]() {
            exists = fileExists(path);
            if (!exists && listDirectory(folder, collectBlocks, blocks)) {
                for (int i = 0; i < blocks->count; i++) {
                    char blockPath[PATH_MAX_LENGTH];
                    snprintf(blockPath, sizeof(blockPath), "%s/%s", folder, blocks->names[i]);
                    uint8_t header[16] = {};
                    FILE* file = fopen(blockPath, "rb");
                    distance[i] = INT64_MAX;
                    if (file != nullptr && fread(header, 1, sizeof(header), file) == sizeof(header)) {
                        const auto ox = static_cast<int32_t>(header[8] | header[9] << 8 | header[10] << 16 | static_cast<uint32_t>(header[11]) << 24);
                        const auto oy = static_cast<int32_t>(header[12] | header[13] << 8 | header[14] << 16 | static_cast<uint32_t>(header[15]) << 24);
                        // The middle of a block of 15 x 15 tiles
                        const int64_t dx = static_cast<int64_t>(ox) + 15 * 2048 - (static_cast<int64_t>(centre.x) * 4096 + 2048);
                        const int64_t dy = static_cast<int64_t>(oy) + 15 * 2048 - (static_cast<int64_t>(centre.y) * 4096 + 2048);
                        distance[i] = dx * dx + dy * dy;
                    }
                    if (file != nullptr) {
                        fclose(file);
                    }
                }
            }
            return true;
        };
        runOnCard(io, check);
        if (exists) {
            addFile(path, region);
        } else {
            int index[64];
            for (int i = 0; i < blocks->count; i++) {
                index[i] = i;
            }
            std::sort(index, index + blocks->count, [&](int a, int b) { return distance[a] < distance[b]; });
            for (int i = 0; i < blocks->count && searchFileCount < SEARCH_MAX_INDEXES; i++) {
                char blockPath[PATH_MAX_LENGTH];
                snprintf(blockPath, sizeof(blockPath), "%s/%s", folder, blocks->names[index[i]]);
                addFile(blockPath, region);
            }
        }
        delete blocks;
    }
    // The name area and the key pages of the nearest index in PSRAM, when there is room (study: 0.76 MB for Bratislava)
    if (searchFileCount > 0 && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) > 1200 * 1024) {
        const uint32_t started = nowMs();
        bool loaded = false;
        auto load = [&]() { loaded = searchEngine->loadResident(0, true, true); };
        onSearchTask(searchTask, load);
        logInfo("Search: name area and key pages of %s %s in %u ms", searchEngine->regionName(0), loaded ? "loaded" : "not loaded",
            static_cast<unsigned>(nowMs() - started));
    }
    logInfo("Search: %d region indexes", searchFileCount);
    return searchFileCount > 0;
}

void MapView::releaseSearch() {
    if (searchEngine != nullptr) {
        auto drop = [&]() {
            searchEngine->dropResident();
            searchEngine->removeAll();
        };
        onSearchTask(searchTask, drop);
    }
    for (int i = 0; i < searchFileCount; i++) {
        searchFiles[i]->~CardSearchFile();
        largeFree(searchFiles[i]);
        searchFiles[i] = nullptr;
    }
    searchFileCount = 0;
}

SearchOrigin MapView::searchOrigin(bool& fromPosition) const {
    fromPosition = position.hasFix() && position.state() == PositionState::Precise;
    const double lat = fromPosition ? position.fix().latitude : worldYToLat(centerY, zoom);
    const double lon = fromPosition ? position.fix().longitude : worldXToLon(centerX, zoom);
    const RoutePoint point = routePointFrom(lat, lon);
    return { point.x, point.y };
}

void MapView::openSearch() {
    closeMenu();
    sheet.close();
    controls.hide();
    if (!loadSearchIndexes()) {
        showToast("No search index yet: it is made in idle time for downloaded maps");
        return;
    }
    searchSheet.open();
    searchSheet.setTypingKeyboard(input.hasKeyboard());
    int count = 0;
    const SearchCategory* categories = SearchEngine::categories(&count);
    const char* names[SearchSheet::MAX_CATEGORIES];
    count = std::min(count, SearchSheet::MAX_CATEGORIES);
    for (int i = 0; i < count; i++) {
        names[i] = categories[i].name;
    }
    searchSheet.setCategories(names, count);
    showRecentRows();
    layoutSearch();
    dirty = true;
}

void MapView::closeSearch() {
    searchSheet.close();
    releaseSearch();
    syncTouchKeyboard();
    dirty = true;
}

void MapView::layoutSearch() {
    syncTouchKeyboard();
    if (touchKeyboard.isOpen()) {
        searchSheet.layout(width, height - TouchKeyboard::heightFor(width), sheetFont(), true);
    } else {
        searchSheet.layout(width, height, sheetFont());
    }
}

void MapView::showRecentRows() {
    // Static: the map task's stack is small
    static SearchSheet::Row rows[SearchSheet::MAX_RECENT];
    const int count = std::min(recentSearches != nullptr ? recentSearches->count() : 0, SearchSheet::MAX_RECENT);
    bool fromPosition;
    const SearchOrigin origin = searchOrigin(fromPosition);
    static char distances[SearchSheet::MAX_RECENT][16];
    for (int i = 0; i < count; i++) {
        const RecentSearches::Entry& entry = recentSearches->at(i);
        const double metres = std::sqrt(static_cast<double>(entry.x - origin.x) * (entry.x - origin.x) + static_cast<double>(entry.y - origin.y) * (entry.y - origin.y)) *
            routeMetresPerUnit(origin.y);
        rows[i] = { entry.name, entry.type, nullptr, plainDistance(static_cast<uint32_t>(metres), distances[i], sizeof(distances[i])),
            static_cast<float>(std::atan2(static_cast<double>(entry.x - origin.x), static_cast<double>(origin.y - entry.y)) * 180.0 / 3.14159265) -
                bearing };
    }
    searchSheet.setRecent(rows, count);
    searchSheet.setHeader(count > 0 ? "Recent" : "");
}

void MapView::fillSearchRows(int from, int count, bool more) {
    // Static: the map task's stack is small
    static SearchSheet::Row rows[SearchSheet::MAX_ROWS];
    static char distances[SearchSheet::MAX_ROWS][16];
    static char notes[SearchSheet::MAX_ROWS][80];
    static char types[SearchSheet::MAX_ROWS][SEARCH_LINE_BYTES];
    for (int i = 0; i < count; i++) {
        const SearchResult& r = searchResults[from + i];
        searchTypeLine(r.type, r.place, types[i], sizeof(types[i]));
        notes[i][0] = '\0';
        if (r.lowConfidence) {
            snprintf(notes[i], sizeof(notes[i]), "Street estimated from the map");
        } else if (r.villageEstimated) {
            snprintf(notes[i], sizeof(notes[i]), "Village estimated from the map");
        }
        if (r.also[0][0] != '\0') {
            const size_t used = strlen(notes[i]);
            snprintf(notes[i] + used, sizeof(notes[i]) - used, "%sAlso: %s%s%s", used > 0 ? " \xC2\xB7 " : "", r.also[0],
                r.also[1][0] != '\0' ? ", " : "", r.also[1]);
        }
        rows[i] = { r.name, types[i], notes[i][0] != '\0' ? notes[i] : nullptr, plainDistance(r.distanceM, distances[i], sizeof(distances[i])),
            static_cast<float>(r.bearing) - bearing };
    }
    if (from == 0) {
        searchSheet.setRows(rows, count, more);
    } else {
        searchSheet.appendRows(rows, count, more);
    }
}

void MapView::runSearch() {
    const char* text = searchSheet.text();
    // Results from 3 characters (a typed letter is 1 to 2 bytes)
    int characters = 0;
    for (const char* p = text; *p != '\0'; p++) {
        characters += (static_cast<uint8_t>(*p) & 0xC0) != 0x80 ? 1 : 0;
    }
    searchResultCount = 0;
    if (characters < 3) {
        searchSheet.setRows(nullptr, 0, false);
        showRecentRows();
        return;
    }
    bool fromPosition;
    const SearchOrigin origin = searchOrigin(fromPosition);
    static SearchStats stats;
    stats = {};
    uint32_t readsBefore = 0;
    for (int i = 0; i < searchFileCount; i++) {
        readsBefore += searchFiles[i]->reads;
    }
    const uint32_t started = nowMs();
    int count = 0;
    auto run = [&]() { count = searchEngine->search(text, origin, searchResults, SEARCH_FIRST_PAGE, &stats); };
    onSearchTask(searchTask, run);
    const uint32_t elapsed = nowMs() - started;
    uint32_t reads = 0;
    for (int i = 0; i < searchFileCount; i++) {
        reads += searchFiles[i]->reads;
    }
    searchResultCount = std::max(0, count);
    const bool more = searchEngine->resultCount() > searchResultCount;
    fillSearchRows(0, searchResultCount, more);
    if (searchResultCount == 0) {
        searchSheet.setHeader("No results");
    } else if (stats.header[0] != '\0') {
        searchSheet.setHeader(stats.header);
    } else {
        searchSheet.setHeader(fromPosition ? "" : "Distances from the map centre");
    }
    scriptLog("Search '%s': %d of %d results in %u ms, %u card reads, engine %u us, search stack left %u", text, searchResultCount,
        searchEngine->resultCount(), static_cast<unsigned>(elapsed), static_cast<unsigned>(reads - readsBefore), static_cast<unsigned>(stats.elapsedUs),
        static_cast<unsigned>(searchTask.stackLeft()));
}

void MapView::scriptLog(const char* format, ...) {
    static char line[256];
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments);
    va_end(arguments);
    logInfo("%s", line);
    if (!script.isActive()) {
        return;
    }
    // Debug scripts also get the lines in scriptlog.txt with the time: measurements without a serial log
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/scriptlog.txt", rootPath);
    const uint32_t now = nowMs();
    auto append = [&]() {
        FILE* file = fopen(path, "a");
        if (file == nullptr) {
            return false;
        }
        fprintf(file, "%u %s\n", static_cast<unsigned>(now), line);
        fclose(file);
        return true;
    };
    runOnCard(io, append);
}

void MapView::moreSearchResults() {
    if (searchEngine == nullptr || searchResultCount >= SEARCH_MAX_RESULTS) {
        return;
    }
    int count = 0;
    auto run = [&]() { count = searchEngine->more(searchResults + searchResultCount, std::min(SEARCH_FIRST_PAGE, SEARCH_MAX_RESULTS - searchResultCount)); };
    onSearchTask(searchTask, run);
    if (count <= 0) {
        searchSheet.appendRows(nullptr, 0, false);
        return;
    }
    const int from = searchResultCount;
    searchResultCount += count;
    fillSearchRows(from, count, searchEngine->resultCount() > searchResultCount && searchResultCount < SEARCH_MAX_RESULTS);
}

void MapView::searchNearby(int category) {
    bool fromPosition;
    const SearchOrigin origin = searchOrigin(fromPosition);
    static SearchStats stats;
    stats = {};
    const uint32_t started = nowMs();
    int count = 0;
    auto run = [&]() { count = searchEngine->nearby(category, origin, searchResults, std::min(SEARCH_NEARBY, SEARCH_MAX_RESULTS), &stats); };
    onSearchTask(searchTask, run);
    searchResultCount = std::max(0, count);
    fillSearchRows(0, searchResultCount, false);
    searchSheet.showResults();
    searchSheet.setHeader(searchResultCount == 0 ? "Nothing of that kind nearby" : (fromPosition ? "" : "Distances from the map centre"));
    logInfo("Search nearby %d: %d results in %u ms", category, searchResultCount, static_cast<unsigned>(nowMs() - started));
}

void MapView::openSearchResult(const char* name, const char* type, int32_t x, int32_t y, const int32_t* box, const char* note) {
    double lat;
    double lon;
    routePointTo({ x, y }, lat, lon);
    int targetZoom = 17;
    if (box != nullptr) {
        // A street: zoomed so its box fits the screen
        const double metres = std::max(box[2] - box[0], box[3] - box[1]) * routeMetresPerUnit(y);
        const double metresPerPixel = metres / (std::min(width, height) * 0.8);
        targetZoom = static_cast<int>(std::floor(std::log(156543.03 * std::cos(lat * 3.14159265358979 / 180.0) / std::max(0.3, metresPerPixel)) / std::log(2.0)));
        targetZoom = std::clamp(targetZoom, 12, 18);
    }
    closeSearch();
    following = false;
    centerOn(lat, lon, targetZoom);
    sheetLat = lat;
    sheetLon = lon;
    selectedWaypoint = -1;
    char coordinates[48];
    searchCoordinates(lat, lon, coordinates, sizeof(coordinates));
    char distance[96];
    describeDistance(lat, lon, distance, sizeof(distance));
    if (note != nullptr && note[0] != '\0') {
        const size_t used = strlen(distance);
        snprintf(distance + used, sizeof(distance) - used, "%s%s", used > 0 ? " \xC2\xB7 " : "", note);
    }
    char title[WaypointSheet::NAME_LENGTH];
    snprintf(title, sizeof(title), "%s", name);
    sheet.openPlace(title, type, coordinates, distance);
    sheet.layout(width, height, sheetFont());
    // The place shows above the sheet, not under it
    pan(0.0, sheet.bounds().height / 2.0);
    searchPinLat = lat;
    searchPinLon = lon;
    searchPinShown = true;
    dirty = true;
}

void MapView::chooseSearchRow(int index, bool recent) {
    if (recent) {
        if (recentSearches == nullptr || index < 0 || index >= recentSearches->count()) {
            return;
        }
        const RecentSearches::Entry entry = recentSearches->at(index);
        openSearchResult(entry.name, entry.type, entry.x, entry.y, nullptr, nullptr);
        return;
    }
    if (index < 0 || index >= searchResultCount) {
        return;
    }
    const SearchResult& result = searchResults[index];
    recentSearches->add(result);
    saveRecentSearches();
    const int32_t box[4] = { result.minX, result.minY, result.maxX, result.maxY };
    const char* note = result.lowConfidence ? "Street estimated from the map" : (result.villageEstimated ? "Village estimated from the map" : nullptr);
    char type[SEARCH_LINE_BYTES];
    searchTypeLine(result.type, result.place, type, sizeof(type));
    openSearchResult(result.name, type, result.x, result.y, result.hasBox ? box : nullptr, note);
}

void MapView::handleSearchEvent(SearchSheet::Event event) {
    dirty = true;
    switch (event.type) {
        case SearchSheet::EventType::TextChanged:
            runSearch();
            break;
        case SearchSheet::EventType::OpenRow:
            chooseSearchRow(event.index, false);
            return;
        case SearchSheet::EventType::OpenRecent:
            chooseSearchRow(event.index, true);
            return;
        case SearchSheet::EventType::OpenCategory:
            searchNearby(event.index);
            break;
        case SearchSheet::EventType::WantMore:
            moreSearchResults();
            break;
        case SearchSheet::EventType::Close:
            closeSearch();
            return;
        default:
            break;
    }
    if (searchSheet.isOpen() && searchSheet.inCategories()) {
        searchSheet.setHeader("");
    }
    if (searchSheet.isOpen()) {
        layoutSearch();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Recent searches: a small text file in the app's folder

namespace {

struct RecentWriter {
    char* buffer;
    size_t size;
    size_t used;
};

bool recentWrite(void* context, const void* data, size_t size) {
    auto* writer = static_cast<RecentWriter*>(context);
    if (writer->used + size > writer->size) {
        return false;
    }
    memcpy(writer->buffer + writer->used, data, size);
    writer->used += size;
    return true;
}

struct RecentReader {
    const uint8_t* data;
    size_t size;
    size_t at;
};

size_t recentRead(void* context, void* out, size_t capacity) {
    auto* reader = static_cast<RecentReader*>(context);
    const size_t count = std::min(capacity, reader->size - reader->at);
    memcpy(out, reader->data + reader->at, count);
    reader->at += count;
    return count;
}

} // namespace

void MapView::loadRecentSearches() {
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/%s", rootPath, RECENT_FILE);
    FileBuffer file;
    auto read = [&]() { return io.readFile(path, file); };
    if (!runOnCard(io, read)) {
        return;
    }
    RecentReader reader { file.bytes(), file.size(), 0 };
    recentSearches->load(recentRead, &reader);
}

void MapView::saveRecentSearches() {
    static char text[4096];
    RecentWriter writer { text, sizeof(text), 0 };
    if (!recentSearches->save(recentWrite, &writer)) {
        return;
    }
    char path[PATH_MAX_LENGTH];
    char temporary[PATH_MAX_LENGTH + 8];
    snprintf(path, sizeof(path), "%s/%s", rootPath, RECENT_FILE);
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    auto write = [&]() { return io.writeFile(path, temporary, reinterpret_cast<const uint8_t*>(text), writer.used, false); };
    if (!runOnCard(io, write)) {
        logError("Search: recent searches not saved");
    }
}

// ------------------------------------------------------------------------------------------------ index build (M6)

namespace {

/** The map must be left alone this long before an index build starts. */
constexpr uint32_t SEARCH_INDEX_IDLE_MS = 20000;
/** After a failed build, the next try waits this long. */
constexpr uint32_t SEARCH_INDEX_RETRY_MS = 10 * 60 * 1000;
/** PSRAM the build needs free when it starts (its peak is about 0.8 MB for Bratislava on the PC). */
constexpr size_t SEARCH_INDEX_MIN_FREE = 1100 * 1024;
constexpr size_t SEARCH_INDEX_MIN_BLOCK = 256 * 1024;

uint32_t crc32Update(uint32_t crc, const uint8_t* data, size_t size) {
    crc = ~crc;
    for (size_t i = 0; i < size; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

} // namespace

void MapView::startSearchIndex(bool force, const char* only) {
    if (searchIndexJob.state() == SearchIndexJob::State::Busy) {
        return;
    }
    searchIndexJob.acknowledge();
    // The region under the map centre first
    const TileKey centre = tileAt(worldYToLat(centerY, zoom), worldXToLon(centerX, zoom), 14);
    const char* first = nullptr;
    for (int i = 0; i < store.regionCount() && first == nullptr; i++) {
        if (store.mayHave(i, centre)) {
            first = store.regionName(i);
        }
    }
    if (searchIndexJob.start(io, rootPath, first, force, only)) {
        scriptLog("Search index build started%s (PSRAM %u KB free, largest block %u KB)", force ? " (force)" : "",
                  static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                  static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
    }
    searchIndexDue = false;
}

void MapView::pollSearchIndex(uint32_t now) {
    // A download or an update that ended may have brought new tiles
    const Downloader::State downloadState = lastStatus.state;
    const bool downloading = downloadState == Downloader::State::Downloading || downloadState == Downloader::State::Updating;
    const bool wasDownloading =
        searchIndexDownloadState == Downloader::State::Downloading || searchIndexDownloadState == Downloader::State::Updating;
    if (wasDownloading && !downloading) {
        searchIndexDue = true;
    }
    searchIndexDownloadState = downloadState;
    // Reports for the log (measurements)
    while (const char* line = searchIndexJob.nextReport()) {
        scriptLog("%s", line);
    }
    switch (searchIndexJob.state()) {
        case SearchIndexJob::State::Busy:
            // Downloads come first: the build waits with its files closed
            searchIndexJob.pause(downloading);
            return;
        case SearchIndexJob::State::Done:
        case SearchIndexJob::State::Failed: {
            const bool failed = searchIndexJob.state() == SearchIndexJob::State::Failed;
            scriptLog("Search index build %s, lowest free PSRAM %u KB", failed ? "failed" : "done",
                      static_cast<unsigned>(searchIndexJob.lowestFreePsram() / 1024));
            if (searchIndexJob.builtAny() && !searchSheet.isOpen()) {
                // The next search opens the new indexes
                releaseSearch();
            }
            searchIndexJob.acknowledge();
            if (failed) {
                searchIndexRetryAt = now + SEARCH_INDEX_RETRY_MS;
                searchIndexDue = true;
            }
            return;
        }
        case SearchIndexJob::State::Idle:
            break;
    }
    if (!searchIndexDue || script.isActive() || static_cast<int32_t>(now - searchIndexRetryAt) < 0 || now - lastInputAt < SEARCH_INDEX_IDLE_MS ||
        downloading || navigating || worker.isPreparing()) {
        return;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_SPIRAM) < SEARCH_INDEX_MIN_FREE ||
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < SEARCH_INDEX_MIN_BLOCK) {
        return;
    }
    startSearchIndex(false);
}

void MapView::checkFile(const char* name) {
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/%s", rootPath, name);
    uint32_t crc = 0;
    uint32_t size = 0;
    bool ok = false;
    auto job = [&]() {
        FILE* file = fopen(path, "rb");
        if (file == nullptr) {
            return false;
        }
        static uint8_t chunk[4096];
        size_t n = 0;
        while ((n = fread(chunk, 1, sizeof(chunk), file)) > 0) {
            crc = crc32Update(crc, chunk, n);
            size += static_cast<uint32_t>(n);
        }
        fclose(file);
        ok = true;
        return true;
    };
    runOnCard(io, job);
    if (ok) {
        scriptLog("File %s: %u bytes, CRC-32 %08x", name, static_cast<unsigned>(size), static_cast<unsigned>(crc));
    } else {
        scriptLog("File %s: cannot read", name);
    }
}

} // namespace maps
