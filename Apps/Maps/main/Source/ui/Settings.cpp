// SPDX-License-Identifier: GPL-3.0-or-later
#include "Settings.h"

#include "../tiles/TileMath.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

struct Entry {
    const char* key;
    bool Settings::*value;
};

constexpr Entry ENTRIES[] = {
    { "sd_cache", &Settings::sdCache },
    { "deflate_prepared", &Settings::deflatePrepared },
    { "smooth_pinch", &Settings::smoothPinch },
    { "browse_download", &Settings::browseDownload },
    { "dev_mono", &Settings::devMonochrome },
    { "dev_probe", &Settings::devProbe },
};

constexpr double MICRODEGREES = 1e6;

/** Writes text to path through a .tmp file, so a reset never leaves half a file. */
bool writeTextFile(const char* path, const char* text) {
    char temporary[160];
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

bool readTextFile(const char* path, char* text, size_t size) {
    FILE* file = fopen(path, "r");
    if (file == nullptr) {
        return false;
    }
    const size_t length = fread(text, 1, size - 1, file);
    fclose(file);
    text[length] = '\0';
    return true;
}

} // namespace

void parseSettings(const char* text, Settings& settings) {
    while (*text != '\0') {
        const char* lineEnd = strchr(text, '\n');
        const size_t length = lineEnd != nullptr ? static_cast<size_t>(lineEnd - text) : strlen(text);
        for (const Entry& entry : ENTRIES) {
            const size_t keyLength = strlen(entry.key);
            if (length > keyLength + 1 && strncmp(text, entry.key, keyLength) == 0 && text[keyLength] == '=') {
                const char value = text[keyLength + 1];
                if (value == '0' || value == '1') {
                    settings.*entry.value = value == '1';
                }
            }
        }
        text += length;
        if (*text == '\n') {
            text++;
        }
    }
}

bool formatSettings(const Settings& settings, char* out, size_t size) {
    size_t used = 0;
    for (const Entry& entry : ENTRIES) {
        const int written = snprintf(out + used, size - used, "%s=%d\n", entry.key, settings.*entry.value ? 1 : 0);
        if (written < 0 || static_cast<size_t>(written) >= size - used) {
            return false;
        }
        used += static_cast<size_t>(written);
    }
    return true;
}

bool readSettings(const char* path, Settings& settings) {
    char text[512];
    if (!readTextFile(path, text, sizeof(text))) {
        return false;
    }
    parseSettings(text, settings);
    return true;
}

bool writeSettings(const char* path, const Settings& settings) {
    char text[512];
    return formatSettings(settings, text, sizeof(text)) && writeTextFile(path, text);
}

bool parseSavedView(const char* text, SavedView& view) {
    char* end = nullptr;
    const long lat = strtol(text, &end, 10);
    if (end == text) {
        return false;
    }
    const char* next = end;
    const long lon = strtol(next, &end, 10);
    if (end == next) {
        return false;
    }
    next = end;
    const long zoom = strtol(next, &end, 10);
    if (end == next || lat < -85000000 || lat > 85000000 || lon < -180000000 || lon > 180000000 || zoom < 0 ||
        zoom > MAX_VIEW_ZOOM) {
        return false;
    }
    view.lat = static_cast<double>(lat) / MICRODEGREES;
    view.lon = static_cast<double>(lon) / MICRODEGREES;
    view.zoom = static_cast<int>(zoom);
    return true;
}

bool formatSavedView(const SavedView& view, char* out, size_t size) {
    const long lat = static_cast<long>(view.lat * MICRODEGREES + (view.lat < 0 ? -0.5 : 0.5));
    const long lon = static_cast<long>(view.lon * MICRODEGREES + (view.lon < 0 ? -0.5 : 0.5));
    const int written = snprintf(out, size, "%ld %ld %d\n", lat, lon, view.zoom);
    return written > 0 && static_cast<size_t>(written) < size;
}

bool readSavedView(const char* path, SavedView& view) {
    char text[64];
    return readTextFile(path, text, sizeof(text)) && parseSavedView(text, view);
}

bool writeSavedView(const char* path, const SavedView& view) {
    char text[64];
    return formatSavedView(view, text, sizeof(text)) && writeTextFile(path, text);
}

} // namespace maps
