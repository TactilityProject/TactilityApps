// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileJson.h"

#include <cstdio>
#include <cstring>

namespace maps {

bool parseTileJson(const char* text, char* urlTemplate, size_t templateSize, char* version, size_t versionSize) {
    const char* tiles = strstr(text, "\"tiles\"");
    if (tiles == nullptr) {
        return false;
    }
    const char* open = strchr(tiles, '[');
    if (open == nullptr) {
        return false;
    }
    const char* start = strchr(open, '"');
    if (start == nullptr) {
        return false;
    }
    start++;
    const char* end = strchr(start, '"');
    if (end == nullptr || static_cast<size_t>(end - start) >= templateSize) {
        return false;
    }
    const size_t length = static_cast<size_t>(end - start);
    memcpy(urlTemplate, start, length);
    urlTemplate[length] = '\0';
    // Only https URLs with all three placeholders are accepted
    if (strncmp(urlTemplate, "https://", 8) != 0 || strstr(urlTemplate, "{z}") == nullptr ||
        strstr(urlTemplate, "{x}") == nullptr || strstr(urlTemplate, "{y}") == nullptr ||
        strstr(urlTemplate, "openstreetmap.org") != nullptr) {
        return false;
    }
    version[0] = '\0';
    const char* placeholder = strstr(urlTemplate, "/{z}");
    if (placeholder != nullptr) {
        const char* segment = placeholder;
        while (segment > urlTemplate && segment[-1] != '/') {
            segment--;
        }
        const size_t versionLength = static_cast<size_t>(placeholder - segment);
        if (versionLength < versionSize) {
            memcpy(version, segment, versionLength);
            version[versionLength] = '\0';
        }
    }
    return true;
}

bool formatTileUrl(const char* urlTemplate, const TileKey& tile, char* out, size_t size) {
    size_t used = 0;
    for (const char* p = urlTemplate; *p != '\0';) {
        int written = 0;
        if (strncmp(p, "{z}", 3) == 0) {
            written = snprintf(out + used, size - used, "%d", tile.z);
            p += 3;
        } else if (strncmp(p, "{x}", 3) == 0) {
            written = snprintf(out + used, size - used, "%d", tile.x);
            p += 3;
        } else if (strncmp(p, "{y}", 3) == 0) {
            written = snprintf(out + used, size - used, "%d", tile.y);
            p += 3;
        } else {
            if (used + 1 >= size) {
                return false;
            }
            out[used] = *p++;
            written = 1;
        }
        if (written <= 0 || used + static_cast<size_t>(written) >= size) {
            return false;
        }
        used += static_cast<size_t>(written);
    }
    out[used] = '\0';
    return true;
}

} // namespace maps
