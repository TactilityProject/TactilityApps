// SPDX-License-Identifier: GPL-3.0-or-later
#include "RegionMeta.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

const char* findValue(const char* text, const char* key) {
    char pattern[32];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    const char* found = strstr(text, pattern);
    if (found == nullptr) {
        return nullptr;
    }
    found = strchr(found + strlen(pattern), ':');
    if (found == nullptr) {
        return nullptr;
    }
    found++;
    while (*found == ' ' || *found == '\t' || *found == '\n' || *found == '\r') {
        found++;
    }
    return found;
}

bool readString(const char* text, const char* key, char* out, size_t size) {
    const char* value = findValue(text, key);
    if (value == nullptr || *value != '"') {
        return false;
    }
    value++;
    size_t length = 0;
    while (value[length] != '"' && value[length] != '\0') {
        length++;
    }
    if (value[length] != '"' || length >= size) {
        return false;
    }
    memcpy(out, value, length);
    out[length] = '\0';
    return true;
}

bool readInteger(const char* text, const char* key, int64_t& out) {
    const char* value = findValue(text, key);
    if (value == nullptr) {
        return false;
    }
    const bool negative = *value == '-';
    if (negative) {
        value++;
    }
    if (*value < '0' || *value > '9') {
        return false;
    }
    int64_t number = 0;
    while (*value >= '0' && *value <= '9') {
        number = number * 10 + (*value - '0');
        value++;
    }
    out = negative ? -number : number;
    return true;
}

} // namespace

bool formatRegionMeta(const RegionMeta& meta, char* out, size_t size) {
    const int written = snprintf(out, size,
        "{\n"
        "  \"name\": \"%s\",\n"
        "  \"bbox\": [%.6f,%.6f,%.6f,%.6f],\n"
        "  \"minzoom\": %d,\n"
        "  \"maxzoom\": %d,\n"
        "  \"source\": \"%s\",\n"
        "  \"version\": \"%s\",\n"
        "  \"date\": \"%s\",\n"
        "  \"total\": %" PRId64 ",\n"
        "  \"done\": %" PRId64 ",\n"
        "  \"bytes\": %" PRIu64 "\n"
        "}\n",
        meta.name, meta.box.west, meta.box.south, meta.box.east, meta.box.north, meta.minZoom, meta.maxZoom,
        meta.source, meta.version, meta.date, meta.total, meta.done, meta.bytes);
    return written > 0 && static_cast<size_t>(written) < size;
}

bool parseRegionMeta(const char* text, RegionMeta& meta) {
    meta = {};
    const char* bbox = findValue(text, "bbox");
    if (bbox == nullptr || sscanf(bbox, "[%lf,%lf,%lf,%lf]", &meta.box.west, &meta.box.south, &meta.box.east, &meta.box.north) != 4) {
        return false;
    }
    int64_t minZoom = 0;
    int64_t maxZoom = 0;
    if (!readInteger(text, "minzoom", minZoom) || !readInteger(text, "maxzoom", maxZoom) ||
        minZoom < 0 || maxZoom > MAX_SOURCE_ZOOM || minZoom > maxZoom) {
        return false;
    }
    meta.minZoom = static_cast<int>(minZoom);
    meta.maxZoom = static_cast<int>(maxZoom);
    readString(text, "name", meta.name, sizeof(meta.name));
    readString(text, "source", meta.source, sizeof(meta.source));
    readString(text, "version", meta.version, sizeof(meta.version));
    readString(text, "date", meta.date, sizeof(meta.date));
    int64_t bytes = 0;
    readInteger(text, "bytes", bytes);
    meta.bytes = static_cast<uint64_t>(bytes);
    if (!readInteger(text, "total", meta.total)) {
        meta.total = tileCount(meta.box, meta.minZoom, meta.maxZoom);
    }
    if (!readInteger(text, "done", meta.done)) {
        meta.done = 0;
    }
    return true;
}

bool writeRegionMeta(const char* path, const RegionMeta& meta) {
    char text[640];
    if (!formatRegionMeta(meta, text, sizeof(text))) {
        return false;
    }
    char temporary[PATH_MAX_LENGTH + 8];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE* file = fopen(temporary, "w");
    if (file == nullptr) {
        return false;
    }
    const size_t length = strlen(text);
    const bool ok = fwrite(text, 1, length, file) == length;
    fclose(file);
    remove(path);
    if (!ok || rename(temporary, path) != 0) {
        remove(temporary);
        return false;
    }
    return true;
}

bool readRegionMeta(const char* path, RegionMeta& meta) {
    FILE* file = fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    char text[1024];
    const size_t length = fread(text, 1, sizeof(text) - 1, file);
    fclose(file);
    text[length] = '\0';
    return parseRegionMeta(text, meta);
}

} // namespace maps
