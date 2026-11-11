// SPDX-License-Identifier: GPL-3.0-or-later
#include "BrowsedMove.h"

#include "TilePaths.h"
#include "TileStore.h"

#include "../render/PreparedTile.h"

#include <sys/stat.h>

#include <cstdio>
#include <cstring>

namespace maps {

namespace {

bool exists(const char* path) {
    struct stat info {};
    return stat(path, &info) == 0;
}

/** The source version in a prepared tile's header, "" when it cannot be read. */
void preparedVersion(const char* path, char* out, size_t size) {
    out[0] = '\0';
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return;
    }
    uint8_t bytes[PREPARED_HEADER_SIZE];
    PreparedHeader header {};
    if (fread(bytes, 1, sizeof(bytes), file) == sizeof(bytes) && readPreparedHeader(bytes, sizeof(bytes), header)) {
        snprintf(out, size, "%s", header.sourceVersion);
    }
    fclose(file);
}

} // namespace

BrowsedMove moveBrowsedFiles(const char* root, const char* region, const TileKey& tile, const char* version,
    const char* browsedVersion, uint64_t& size, bool (*makeFolder)(const char* path)) {
    size = 0;
    char fromOriginal[PATH_MAX_LENGTH];
    char fromPrepared[PATH_MAX_LENGTH];
    char toOriginal[PATH_MAX_LENGTH];
    char toPrepared[PATH_MAX_LENGTH];
    if (!tilePath(fromOriginal, sizeof(fromOriginal), root, BROWSE_REGION, tile) ||
        !preparedPath(fromPrepared, sizeof(fromPrepared), root, BROWSE_REGION, tile) ||
        !tilePath(toOriginal, sizeof(toOriginal), root, region, tile) ||
        !preparedPath(toPrepared, sizeof(toPrepared), root, region, tile)) {
        return BrowsedMove::Failed;
    }
    struct stat info {};
    if (stat(fromOriginal, &info) != 0) {
        return BrowsedMove::Failed;
    }
    size = static_cast<uint64_t>(info.st_size);
    const bool hasPrepared = exists(fromPrepared);
    char tileVersion[PREPARED_VERSION_LENGTH + 1] = "";
    if (hasPrepared) {
        preparedVersion(fromPrepared, tileVersion, sizeof(tileVersion));
    }
    if (tileVersion[0] == '\0' && browsedVersion != nullptr) {
        snprintf(tileVersion, sizeof(tileVersion), "%s", browsedVersion);
    }
    if (version != nullptr && version[0] != '\0' && tileVersion[0] != '\0' && strcmp(version, tileVersion) != 0) {
        return BrowsedMove::Stale;
    }
    char folder[PATH_MAX_LENGTH];
    snprintf(folder, sizeof(folder), "%s/maps/%s/%d", root, region, tile.z);
    makeFolder(folder);
    if (tileFolderPath(folder, sizeof(folder), root, region, tile)) {
        makeFolder(folder);
    }
    if (hasPrepared) {
        // A prepared copy already in the region is from an interrupted move or an old download: it stays
        if (exists(toPrepared)) {
            remove(fromPrepared);
        } else if (rename(fromPrepared, toPrepared) != 0) {
            return BrowsedMove::Failed;
        }
    }
    // The label and graph files follow, or are made again there when the move cannot take them
    for (int file = 1; file < PREPARED_FILE_COUNT; file++) {
        char from[PATH_MAX_LENGTH];
        char to[PATH_MAX_LENGTH];
        if (preparedFilePath(from, sizeof(from), root, BROWSE_REGION, tile, static_cast<PreparedFile>(file)) &&
            preparedFilePath(to, sizeof(to), root, region, tile, static_cast<PreparedFile>(file)) && exists(from) &&
            (exists(to) || rename(from, to) != 0)) {
            remove(from);
        }
    }
    if (rename(fromOriginal, toOriginal) != 0) {
        return BrowsedMove::Failed;
    }
    return BrowsedMove::Moved;
}

uint64_t removeBrowsedFiles(const char* root, const TileKey& tile) {
    char path[PATH_MAX_LENGTH];
    for (int file = 0; file < PREPARED_FILE_COUNT; file++) {
        if (preparedFilePath(path, sizeof(path), root, BROWSE_REGION, tile, static_cast<PreparedFile>(file))) {
            remove(path);
        }
    }
    struct stat info {};
    if (tilePath(path, sizeof(path), root, BROWSE_REGION, tile) && stat(path, &info) == 0 && remove(path) == 0) {
        return static_cast<uint64_t>(info.st_size);
    }
    return 0;
}

} // namespace maps
