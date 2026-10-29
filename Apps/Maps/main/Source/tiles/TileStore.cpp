// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileStore.h"

#include "../platform/Platform.h"
#include "RegionMeta.h"

#include <cstdio>
#include <cstring>

namespace maps {

namespace {

bool addRegionEntry(const char* name, bool isDirectory, void* context) {
    if (!isDirectory) {
        return true;
    }
    char lower[REGION_NAME_MAX + 1];
    const size_t length = strlen(name);
    if (length > REGION_NAME_MAX) {
        return true;
    }
    // FAT reports short names in upper case
    for (size_t i = 0; i <= length; i++) {
        const char c = name[i];
        lower[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    static_cast<TileStore*>(context)->addRegion(lower);
    return true;
}

} // namespace

void TileStore::setRoot(const char* path) {
    snprintf(rootPath, sizeof(rootPath), "%s", path);
    count = 0;
}

bool TileStore::addRegion(const char* name) {
    if (count >= MAX_REGIONS || !isValidRegionName(name)) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(regions[i], name) == 0) {
            return true;
        }
    }
    snprintf(regions[count], sizeof(regions[count]), "%s", name);
    versions[count][0] = '\0';
    bounds[count] = {};
    count++;
    return true;
}

void TileStore::removeRegion(const char* name) {
    for (int i = 0; i < count; i++) {
        if (strcmp(regions[i], name) == 0) {
            for (int j = i + 1; j < count; j++) {
                memcpy(regions[j - 1], regions[j], sizeof(regions[j]));
                memcpy(versions[j - 1], versions[j], sizeof(versions[j]));
                bounds[j - 1] = bounds[j];
            }
            count--;
            return;
        }
    }
}

int TileStore::scanRegions() {
    count = 0;
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/maps", rootPath);
    listDirectory(path, addRegionEntry, this);
    for (int i = 0; i < count; i++) {
        snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, regions[i]);
        RegionMeta meta {};
        if (readRegionMeta(path, meta)) {
            snprintf(versions[i], sizeof(versions[i]), "%s", meta.version);
            setRegionBounds(regions[i], meta.box, meta.minZoom, meta.maxZoom);
        }
    }
    return count;
}

void TileStore::setRegionVersion(const char* name, const char* version) {
    for (int i = 0; i < count; i++) {
        if (strcmp(regions[i], name) == 0) {
            snprintf(versions[i], sizeof(versions[i]), "%s", version);
        }
    }
}

void TileStore::setRegionBounds(const char* name, const BoundingBox& box, int minZoom, int maxZoom) {
    if (strcmp(name, BROWSE_REGION) == 0) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(regions[i], name) == 0) {
            const bool allZooms = minZoom > maxZoom;
            bounds[i] = { true, static_cast<int8_t>(allZooms ? 0 : minZoom), static_cast<int8_t>(allZooms ? MAX_SOURCE_ZOOM : maxZoom), box };
        }
    }
}

bool TileStore::mayHave(int index, const TileKey& tile) const {
    const Bounds& b = bounds[index];
    if (!b.known) {
        return true;
    }
    // Without an area the bounds say nothing, as for an old or damaged meta.json
    if (!(b.box.west < b.box.east && b.box.south < b.box.north)) {
        return true;
    }
    if (tile.z < b.minZoom || tile.z > b.maxZoom) {
        return false;
    }
    // One tile of slack around the box, tools that wrote the region may round its edges differently
    const TileRange range = tileRange(b.box, tile.z);
    return tile.x >= range.minX - 1 && tile.x <= range.maxX + 1 && tile.y >= range.minY - 1 && tile.y <= range.maxY + 1;
}

bool TileStore::find(const TileKey& target, TileFile& file) const {
    const int top = target.z < MAX_SOURCE_ZOOM ? target.z : MAX_SOURCE_ZOOM;
    const int bottom = target.z - MAX_OVERZOOM > 0 ? target.z - MAX_OVERZOOM : 0;
    for (int z = top; z >= bottom; z--) {
        const TileKey candidate = ancestorOf(target, z);
        for (int i = 0; i < count; i++) {
            if (!mayHave(i, candidate)) {
                continue;
            }
            if (tilePath(file.path, sizeof(file.path), rootPath, regions[i], candidate) && fileSize(file.path, file.size)) {
                file.tile = candidate;
                file.region = i;
                return true;
            }
        }
    }
    return false;
}

bool TileStore::find(const TileKey& target, TileKey& source, char* path, size_t pathSize) const {
    TileFile file {};
    if (!find(target, file) || strlen(file.path) >= pathSize) {
        return false;
    }
    source = file.tile;
    snprintf(path, pathSize, "%s", file.path);
    return true;
}

} // namespace maps
