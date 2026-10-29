// SPDX-License-Identifier: GPL-3.0-or-later
#include "CacheWalker.h"

#include "../platform/Platform.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

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

struct TopList {
    char (*names)[16];
    int count;
    int capacity;
};

bool addTop(const char* name, bool isDirectory, void* context) {
    auto* list = static_cast<TopList*>(context);
    if (isDirectory && strlen(name) < 16 && list->count < list->capacity) {
        // FAT may report short names in upper case
        char* out = list->names[list->count++];
        for (size_t i = 0; i <= strlen(name); i++) {
            out[i] = (name[i] >= 'A' && name[i] <= 'Z') ? static_cast<char>(name[i] - 'A' + 'a') : name[i];
        }
    }
    return true;
}

struct ColumnList {
    int* columns;
    int count;
    int capacity;
};

bool addColumn(const char* name, bool isDirectory, void* context) {
    auto* list = static_cast<ColumnList*>(context);
    if (isDirectory && isNumber(name) && list->count < list->capacity) {
        list->columns[list->count++] = atoi(name);
    }
    return true;
}

} // namespace

CacheWalker::~CacheWalker() {
    largeFree(columns);
}

void CacheWalker::begin(const char* cacheRoot, const char* styleId, bool keepCurrent) {
    snprintf(root, sizeof(root), "%s", cacheRoot);
    snprintf(style, sizeof(style), "%s", styleId);
    keep = keepCurrent;
    phase = Phase::ListTop;
    topCount = 0;
    topIndex = 0;
    zoom = -1;
    columnCount = -1;
    column = 0;
}

bool CacheWalker::removeInCache(const char* path, bool folder) {
    // Everything outside the cache folder, maps/ above all, is never deleted here
    if (!isInsideCache(path, root)) {
        logError("Refusing to delete %s outside the cache", path);
        return false;
    }
    return folder ? removeFolder(path) : removeFile(path);
}

bool CacheWalker::listColumns(const char* folder) {
    if (columns == nullptr) {
        columns = static_cast<int*>(largeAlloc(MAX_COLUMNS * sizeof(int)));
        if (columns == nullptr) {
            return false;
        }
    }
    ColumnList list { columns, 0, MAX_COLUMNS };
    listDirectory(folder, addColumn, &list);
    columnCount = list.count;
    column = 0;
    return true;
}

void CacheWalker::indexColumn(CacheIndex& index, const IndexLock* lock, int z, int x) {
    char folder[PATH_MAX_LENGTH];
    snprintf(folder, sizeof(folder), "%s/cache/%s/%d/%d", root, style, z, x);
    struct Visit {
        CacheIndex* index;
        const IndexLock* lock;
        const char* folder;
        int z;
        int x;
    } visit { &index, lock, folder, z, x };
    listDirectory(folder, [](const char* name, bool isDirectory, void* context) {
        auto* v = static_cast<Visit*>(context);
        if (isDirectory || !isNumber(name)) {
            return true;
        }
        char path[PATH_MAX_LENGTH];
        uint32_t size = 0;
        snprintf(path, sizeof(path), "%s/%s", v->folder, name);
        const TileKey tile { v->z, v->x, atoi(name) };
        if (!fileSize(path, size)) {
            return true;
        }
        if (v->lock != nullptr) {
            v->lock->lock(v->lock->context);
        }
        // Files already in the index keep their last use
        if (!v->index->contains(tile)) {
            v->index->put(tile, size);
        }
        if (v->lock != nullptr) {
            v->lock->unlock(v->lock->context);
        }
        return true;
    }, &visit);
}

bool CacheWalker::deleteColumn(const char* zoomFolder, int x) {
    char folder[PATH_MAX_LENGTH];
    snprintf(folder, sizeof(folder), "%s/%d", zoomFolder, x);
    struct Visit {
        CacheWalker* walker;
        const char* folder;
        int removed;
    } visit { this, folder, 0 };
    listDirectory(folder, [](const char* name, bool isDirectory, void* context) {
        auto* v = static_cast<Visit*>(context);
        if (!isDirectory) {
            char path[PATH_MAX_LENGTH];
            snprintf(path, sizeof(path), "%s/%s", v->folder, name);
            v->walker->removeInCache(path, false);
            v->removed++;
        }
        // A few files per step, so the card is free again soon for the tiles on screen
        return v->removed < MAX_DELETES_PER_STEP;
    }, &visit);
    if (visit.removed >= MAX_DELETES_PER_STEP) {
        return false;
    }
    removeInCache(folder, true);
    return true;
}

bool CacheWalker::step(CacheIndex& index, const IndexLock* lock) {
    while (phase != Phase::Done) {
        if (phase == Phase::ListTop) {
            char folder[PATH_MAX_LENGTH];
            snprintf(folder, sizeof(folder), "%s/cache", root);
            TopList list { top, 0, MAX_TOP };
            listDirectory(folder, addTop, &list);
            topCount = list.count;
            topIndex = 0;
            zoom = -1;
            columnCount = -1;
            phase = Phase::Top;
            return true;
        }
        if (topIndex >= topCount) {
            // The index of the current style matches the card now, also when it has no folder yet
            index.setComplete(true);
            phase = Phase::Done;
            break;
        }
        const char* name = top[topIndex];
        const bool current = keep && strcmp(name, style) == 0;
        const bool legacy = isNumber(name);
        if (current && index.isComplete()) {
            topIndex++;
            continue;
        }
        // The old layout has zoom folders right under cache/, a style folder has them one level down
        if (zoom < 0) {
            zoom = 0;
        }
        if (zoom > MAX_VIEW_ZOOM) {
            char path[PATH_MAX_LENGTH];
            if (current) {
                index.setComplete(true);
            } else {
                if (!legacy) {
                    snprintf(path, sizeof(path), "%s/cache/%s/index.bin", root, name);
                    removeInCache(path, false);
                }
                snprintf(path, sizeof(path), "%s/cache/%s", root, name);
                removeInCache(path, true);
            }
            topIndex++;
            zoom = -1;
            columnCount = -1;
            continue;
        }
        char zoomFolder[PATH_MAX_LENGTH];
        if (legacy) {
            snprintf(zoomFolder, sizeof(zoomFolder), "%s/cache/%s", root, name);
        } else {
            snprintf(zoomFolder, sizeof(zoomFolder), "%s/cache/%s/%d", root, name, zoom);
        }
        if (columnCount < 0 && !listColumns(zoomFolder)) {
            return false;
        }
        if (column < columnCount) {
            if (current) {
                indexColumn(index, lock, zoom, columns[column]);
            } else {
                if (!deleteColumn(zoomFolder, columns[column])) {
                    return true;
                }
            }
            column++;
            return true;
        }
        if (!current && !legacy) {
            removeInCache(zoomFolder, true);
        }
        // The old layout has a single zoom folder: moving past it ends this entry
        zoom = legacy ? MAX_VIEW_ZOOM + 1 : zoom + 1;
        columnCount = -1;
    }
    return false;
}

} // namespace maps
