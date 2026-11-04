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
        "  \"bytes\": %" PRIu64 ",\n"
        "  \"outline\": \"%s\",\n"
        "  \"title\": \"%s\"\n"
        "}\n",
        meta.name, meta.box.west, meta.box.south, meta.box.east, meta.box.north, meta.minZoom, meta.maxZoom,
        meta.source, meta.version, meta.date, meta.total, meta.done, meta.bytes, meta.outline, meta.title);
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
    // Written last, so a title that looks like a key cannot be taken for one of the keys above
    readString(text, "outline", meta.outline, sizeof(meta.outline));
    readString(text, "title", meta.title, sizeof(meta.title));
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

void setRegionTitle(RegionMeta& meta, const char* title) {
    // Quotes and backslashes would end the JSON string early, the title only needs to read well
    size_t length = 0;
    for (; title[length] != '\0' && length + 1 < sizeof(meta.title); length++) {
        const char c = title[length];
        meta.title[length] = c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20 ? '\'' : c;
    }
    // Never end inside a UTF-8 sequence: when the title was cut, drop a last character that lost bytes
    if (title[length] != '\0') {
        size_t lead = length;
        while (lead > 0 && (static_cast<unsigned char>(meta.title[lead - 1]) & 0xC0) == 0x80) {
            lead--;
        }
        if (lead > 0 && (static_cast<unsigned char>(meta.title[lead - 1]) & 0x80) != 0) {
            const auto first = static_cast<unsigned char>(meta.title[lead - 1]);
            const size_t needed = first >= 0xF0 ? 4 : (first >= 0xE0 ? 3 : 2);
            if (length - (lead - 1) < needed) {
                length = lead - 1;
            }
        }
    }
    meta.title[length] = '\0';
}

void regionFolderName(const char* title, int number, char* out, size_t size) {
    char base[REGION_NAME_MAX + 1];
    size_t length = 0;
    for (const char* c = title; *c != '\0' && length < REGION_NAME_MAX; c++) {
        if (*c >= 'A' && *c <= 'Z') {
            base[length++] = static_cast<char>(*c - 'A' + 'a');
        } else if ((*c >= 'a' && *c <= 'z') || (*c >= '0' && *c <= '9')) {
            base[length++] = *c;
        }
    }
    base[length] = '\0';
    if (length == 0) {
        snprintf(base, sizeof(base), "area");
        length = 4;
    }
    if (number <= 0) {
        snprintf(out, size, "%s", base);
        return;
    }
    char suffix[12];
    const int digits = snprintf(suffix, sizeof(suffix), "%d", number);
    const size_t keep = length + static_cast<size_t>(digits) <= REGION_NAME_MAX ? length : REGION_NAME_MAX - static_cast<size_t>(digits);
    snprintf(out, size, "%.*s%s", static_cast<int>(keep), base, suffix);
}

const char* regionTitle(const RegionMeta& meta) {
    return meta.title[0] != '\0' ? meta.title : meta.name;
}

bool writeRegionMeta(const char* path, const RegionMeta& meta) {
    char text[896];
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
