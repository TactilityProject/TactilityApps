// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../core/GnssModel.h"

#include <cstdint>
#include <cstdio>

namespace gpsviewer {

// Writes received GPS events as an NMEA log. App task only.
class Recorder {
public:
    // Creates <dataDirectory>/logs/gps-<UTC date and time>.nmea, or gps-up<uptime>.nmea without a valid time
    bool start(const char* dataDirectory, const UtcDateTime& utc, uint32_t nowMs);
    void stop();
    void write(const GnssEvent& event, uint32_t nowMs);
    bool isRecording() const { return file != nullptr; }
    uint32_t lineCount() const { return lines; }
    const char* fileName() const { return name; }

private:
    void flush();

    FILE* file = nullptr;
    char name[40] = "";
    char buffer[4096] = "";
    size_t used = 0;
    uint32_t lines = 0;
    uint32_t flushedAtMs = 0;
};

} // namespace gpsviewer
