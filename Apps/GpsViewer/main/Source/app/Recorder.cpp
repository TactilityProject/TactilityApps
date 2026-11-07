// SPDX-License-Identifier: GPL-3.0-or-later
#include "Recorder.h"
#include "AppContext.h"

#include "../core/NmeaWriter.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace gpsviewer {

namespace {

// Fewer, larger writes keep the SD card and the shared SPI bus quiet
constexpr uint32_t FLUSH_INTERVAL_MS = 2000;
constexpr size_t MAX_LINE = 128;

bool endsWith(const char* text, const char* suffix) {
    const size_t textLength = strlen(text);
    const size_t suffixLength = strlen(suffix);
    return textLength >= suffixLength && strcmp(text + textLength - suffixLength, suffix) == 0;
}

} // namespace

bool Recorder::start(const char* dataDirectory, const UtcDateTime& utc, uint32_t nowMs) {
    stop();
    if (dataDirectory[0] == '\0') {
        return false;
    }
    char folder[PATH_LENGTH + 8];
    snprintf(folder, sizeof(folder), "%s/logs", dataDirectory);
    mkdir(folder, 0777);
    if (utc.valid) {
        snprintf(name, sizeof(name), "gps-%04d%02d%02d-%02d%02d%02d.nmea", utc.year, utc.month, utc.day, utc.hour, utc.minute, utc.second);
    } else {
        snprintf(name, sizeof(name), "gps-up%lu.nmea", static_cast<unsigned long>(nowMs / 1000));
    }
    char path[PATH_LENGTH + 48];
    snprintf(path, sizeof(path), "%s/%s", folder, name);
    file = fopen(path, "w");
    used = 0;
    lines = 0;
    flushedAtMs = nowMs;
    if (file == nullptr) {
        name[0] = '\0';
        return false;
    }
    return true;
}

void Recorder::flush() {
    if (file != nullptr && used > 0) {
        fwrite(buffer, 1, used, file);
        fflush(file);
        // FAT only commits the data and the file size on sync, so a crash or power loss keeps the log
        fsync(fileno(file));
    }
    used = 0;
}

void Recorder::stop() {
    if (file == nullptr) {
        return;
    }
    flush();
    fclose(file);
    file = nullptr;
}

void Recorder::write(const GnssEvent& event, uint32_t nowMs) {
    if (file == nullptr) {
        return;
    }
    if (sizeof(buffer) - used < MAX_LINE) {
        flush();
    }
    const size_t length = formatNmeaLine(event, buffer + used, sizeof(buffer) - used);
    if (length > 0) {
        used += length;
        lines++;
    }
    if (nowMs - flushedAtMs >= FLUSH_INTERVAL_MS) {
        flush();
        flushedAtMs = nowMs;
    }
}

} // namespace gpsviewer
