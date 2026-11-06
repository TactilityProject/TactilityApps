// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreparedScanner.h"

#include "../platform/Platform.h"
#include "PreparedTile.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

struct ColumnList {
    int* columns;
    int count;
    int capacity;
};

bool isNumber(const char* text) {
    if (*text == '\0') {
        return false;
    }
    for (; *text != '\0'; text++) {
        if (*text < '0' || *text > '9') {
            return false;
        }
    }
    return true;
}

bool addColumn(const char* name, bool isDirectory, void* context) {
    auto* list = static_cast<ColumnList*>(context);
    if (isDirectory && isNumber(name) && list->count < list->capacity) {
        list->columns[list->count++] = atoi(name);
    }
    return true;
}

struct FileVisit {
    const TileStore* store;
    int region;
    int zoom;
    int x;
    PreparedScanner::StaleCallback callback;
    void* context;
    bool stopped;
    /** Originals seen in this folder so far, the first skip of them were looked at in earlier steps. */
    int seen;
    int skip;
    int checks;
};

/** Headers read per step: each is a file open on the card, which holds it for the tiles on screen. */
constexpr int CHECKS_PER_STEP = 4;

bool visitFile(const char* name, bool isDirectory, void* context) {
    auto* visit = static_cast<FileVisit*>(context);
    const size_t length = strlen(name);
    // FAT may report short names in upper case
    if (isDirectory || length < 5 || (strcmp(name + length - 4, ".pbf") != 0 && strcmp(name + length - 4, ".PBF") != 0)) {
        return true;
    }
    char number[16];
    if (length - 4 >= sizeof(number)) {
        return true;
    }
    memcpy(number, name, length - 4);
    number[length - 4] = '\0';
    if (!isNumber(number)) {
        return true;
    }
    if (visit->seen++ < visit->skip) {
        return true;
    }
    if (visit->checks >= CHECKS_PER_STEP) {
        // The rest of the folder comes in the next step
        visit->seen--;
        visit->stopped = true;
        return false;
    }
    visit->checks++;
    const TileKey tile { visit->zoom, visit->x, atoi(number) };
    const TileStore& store = *visit->store;
    char path[PATH_MAX_LENGTH];
    uint32_t size = 0;
    if (!tilePath(path, sizeof(path), store.root(), store.regionName(visit->region), tile) || !fileSize(path, size) ||
        !preparedPath(path, sizeof(path), store.root(), store.regionName(visit->region), tile)) {
        return true;
    }
    if (PreparedScanner::isCurrent(path, tile, size)) {
        return true;
    }
    if (!visit->callback(tile, visit->region, visit->context)) {
        // The queue is full: this tile is looked at again next time
        visit->seen--;
        visit->stopped = true;
        return false;
    }
    return true;
}

} // namespace

PreparedScanner::~PreparedScanner() {
    largeFree(columns);
}

void PreparedScanner::restart() {
    region = 0;
    zoom = 0;
    column = 0;
    columnCount = -1;
    fileSkip = 0;
    complete = false;
}

bool PreparedScanner::isCurrent(const char* preparedPath, const TileKey& tile, uint32_t sourceSize) {
    FILE* file = fopen(preparedPath, "rb");
    if (file == nullptr) {
        return false;
    }
    uint8_t bytes[PREPARED_HEADER_SIZE];
    const bool read = fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes);
    fclose(file);
    PreparedHeader header {};
    return read && readPreparedHeader(bytes, sizeof(bytes), header) && isPreparedCurrent(header, tile, sourceSize);
}

bool PreparedScanner::listColumns(const TileStore& store) {
    if (columns == nullptr) {
        columns = static_cast<int*>(largeAlloc(MAX_COLUMNS * sizeof(int)));
        if (columns == nullptr) {
            return false;
        }
    }
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/maps/%s/%d", store.root(), store.regionName(region), zoom);
    ColumnList list { columns, 0, MAX_COLUMNS };
    listDirectory(path, addColumn, &list);
    columnCount = list.count;
    column = 0;
    return true;
}

bool PreparedScanner::step(const TileStore& store, StaleCallback callback, void* context) {
    while (!complete) {
        if (region >= store.regionCount()) {
            complete = true;
            break;
        }
        if (zoom > MAX_SOURCE_ZOOM) {
            region++;
            zoom = 0;
            columnCount = -1;
            continue;
        }
        if (columnCount < 0 && !listColumns(store)) {
            return false;
        }
        if (column >= columnCount) {
            zoom++;
            columnCount = -1;
            continue;
        }
        char path[PATH_MAX_LENGTH];
        snprintf(path, sizeof(path), "%s/maps/%s/%d/%d", store.root(), store.regionName(region), zoom, columns[column]);
        FileVisit visit { &store, region, zoom, columns[column], callback, context, false, 0, fileSkip, 0 };
        listDirectory(path, visitFile, &visit);
        // A stopped folder goes on next time after the originals already looked at
        if (visit.stopped) {
            fileSkip = visit.seen;
        } else {
            column++;
            fileSkip = 0;
        }
        return true;
    }
    return false;
}

} // namespace maps
