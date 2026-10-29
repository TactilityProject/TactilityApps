// SPDX-License-Identifier: GPL-3.0-or-later
#include "TilePaths.h"

#include <cstdio>
#include <cstring>

namespace maps {

namespace {

bool fits(int written, size_t size) {
    return written > 0 && static_cast<size_t>(written) < size;
}

} // namespace

bool tilePath(char* out, size_t size, const char* root, const char* region, const TileKey& tile) {
    return fits(snprintf(out, size, "%s/maps/%s/%d/%d/%d.pbf", root, region, tile.z, tile.x, tile.y), size);
}

bool preparedPath(char* out, size_t size, const char* root, const char* region, const TileKey& tile) {
    return fits(snprintf(out, size, "%s/maps/%s/%d/%d/%d.prt", root, region, tile.z, tile.x, tile.y), size);
}

bool preparedTempPath(char* out, size_t size, const char* root, const char* region, const TileKey& tile) {
    return fits(snprintf(out, size, "%s/maps/%s/%d/%d/%d.ptm", root, region, tile.z, tile.x, tile.y), size);
}

bool tileFolderPath(char* out, size_t size, const char* root, const char* region, const TileKey& tile) {
    return fits(snprintf(out, size, "%s/maps/%s/%d/%d", root, region, tile.z, tile.x), size);
}

bool cachePath(char* out, size_t size, const char* root, const char* style, const TileKey& tile) {
    return fits(snprintf(out, size, "%s/cache/%s/%d/%d/%d", root, style, tile.z, tile.x, tile.y), size);
}

bool cacheFolderPath(char* out, size_t size, const char* root, const char* style, const TileKey& tile) {
    return fits(snprintf(out, size, "%s/cache/%s/%d/%d", root, style, tile.z, tile.x), size);
}

bool isInsideCache(const char* path, const char* root) {
    const size_t rootLength = strlen(root);
    if (strncmp(path, root, rootLength) != 0 || strncmp(path + rootLength, "/cache/", 7) != 0 || path[rootLength + 7] == '\0') {
        return false;
    }
    // No way back out of the cache folder
    return strstr(path, "/..") == nullptr && strstr(path, "\\") == nullptr;
}

bool isValidRegionName(const char* name) {
    const size_t length = strlen(name);
    if (length == 0 || length > REGION_NAME_MAX) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        const char c = name[i];
        const bool valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!valid) {
            return false;
        }
    }
    return true;
}

} // namespace maps
