// SPDX-License-Identifier: GPL-3.0-or-later

#include "Settings.h"

#include <cstdio>

namespace gpsviewer {

namespace {

bool isBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// True when the range [begin, end) equals the NUL-terminated word
bool sameText(const char* begin, const char* end, const char* word) {
    const char* p = begin;
    while (p < end && *word != '\0') {
        if (*p != *word) {
            return false;
        }
        ++p;
        ++word;
    }
    return p == end && *word == '\0';
}

void applyLine(const char* begin, const char* end, Settings& settings) {
    while (begin < end && isBlank(*begin)) {
        ++begin;
    }
    while (end > begin && isBlank(end[-1])) {
        --end;
    }
    if (begin == end || *begin == '#') {
        return;
    }

    const char* eq = begin;
    while (eq < end && *eq != '=') {
        ++eq;
    }
    if (eq == end) {
        return;
    }

    const char* keyEnd = eq;
    while (keyEnd > begin && isBlank(keyEnd[-1])) {
        --keyEnd;
    }
    const char* value = eq + 1;
    while (value < end && isBlank(*value)) {
        ++value;
    }

    if (sameText(begin, keyEnd, "units")) {
        if (sameText(value, end, "metric")) {
            settings.units = Units::Metric;
        } else if (sameText(value, end, "imperial")) {
            settings.units = Units::Imperial;
        } else if (sameText(value, end, "nautical")) {
            settings.units = Units::Nautical;
        }
    } else if (sameText(begin, keyEnd, "coordinates")) {
        if (sameText(value, end, "dd")) {
            settings.coordinateFormat = CoordinateFormat::Degrees;
        } else if (sameText(value, end, "dm")) {
            settings.coordinateFormat = CoordinateFormat::DegreesMinutes;
        } else if (sameText(value, end, "dms")) {
            settings.coordinateFormat = CoordinateFormat::DegreesMinutesSeconds;
        }
    } else if (sameText(begin, keyEnd, "night")) {
        if (sameText(value, end, "0")) {
            settings.nightMode = false;
        } else if (sameText(value, end, "1")) {
            settings.nightMode = true;
        }
    }
}

const char* unitsName(Units units) {
    switch (units) {
        case Units::Imperial:
            return "imperial";
        case Units::Nautical:
            return "nautical";
        case Units::Metric:
            break;
    }
    return "metric";
}

const char* coordinatesName(CoordinateFormat format) {
    switch (format) {
        case CoordinateFormat::DegreesMinutes:
            return "dm";
        case CoordinateFormat::DegreesMinutesSeconds:
            return "dms";
        case CoordinateFormat::Degrees:
            break;
    }
    return "dd";
}

} // namespace

Settings parseSettings(const char* text) {
    Settings settings;
    if (!text) {
        return settings;
    }
    const char* line = text;
    while (*line != '\0') {
        const char* end = line;
        while (*end != '\0' && *end != '\n') {
            ++end;
        }
        applyLine(line, end, settings);
        line = (*end == '\n') ? end + 1 : end;
    }
    return settings;
}

size_t serializeSettings(const Settings& settings, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return 0;
    }
    const int written = std::snprintf(out, outSize,
                                      "units=%s\ncoordinates=%s\nnight=%d\n",
                                      unitsName(settings.units),
                                      coordinatesName(settings.coordinateFormat),
                                      settings.nightMode ? 1 : 0);
    // A text that does not fit is not written at all, so a partial file never parses to odd values
    if (written < 0 || static_cast<size_t>(written) >= outSize) {
        out[0] = '\0';
        return 0;
    }
    return static_cast<size_t>(written);
}

} // namespace gpsviewer
