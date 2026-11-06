// SPDX-License-Identifier: GPL-3.0-or-later
#include "GpxWriter.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

void civilFromDays(int64_t days, int& year, unsigned& month, unsigned& day) {
    const int64_t shifted = days + 719468;
    const int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(shifted - era * 146097);
    const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    year = static_cast<int>(yearOfEra) + static_cast<int>(era) * 400;
    const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    year += month <= 2;
}

struct TextBuilder {
    char* out;
    size_t size;
    int used = 0;
    bool overflow = false;

    TextBuilder(char* buffer, size_t capacity) : out(buffer), size(capacity) {}

    void add(const char* text) {
        if (overflow) {
            return;
        }
        if (size == 0 || out == nullptr) {
            overflow = true;
            return;
        }
        const size_t length = strlen(text);
        if (static_cast<size_t>(used) + length >= size) {
            overflow = true;
            out[size - 1] = '\0';
            return;
        }
        memcpy(out + used, text, length);
        used += static_cast<int>(length);
        out[used] = '\0';
    }

    void addf(const char* format, ...) {
        if (overflow) {
            return;
        }
        if (size == 0 || out == nullptr || static_cast<size_t>(used) >= size) {
            overflow = true;
            if (out != nullptr && size > 0) {
                out[size - 1] = '\0';
            }
            return;
        }
        va_list args;
        va_start(args, format);
        const int written = vsnprintf(out + used, size - static_cast<size_t>(used), format, args);
        va_end(args);
        if (written < 0 || static_cast<size_t>(written) >= size - static_cast<size_t>(used)) {
            overflow = true;
            out[size - 1] = '\0';
            return;
        }
        used += written;
    }

    int result() const {
        return overflow ? -1 : used;
    }
};

void addPointBody(TextBuilder& builder, const GpxPoint& point, const char* tag) {
    builder.addf("<%s lat=\"%.7f\" lon=\"%.7f\">", tag, point.lat, point.lon);
    if (point.elevation == point.elevation) {
        builder.addf("<ele>%.1f</ele>", static_cast<double>(point.elevation));
    }
    if (point.time >= 0) {
        char time[32];
        formatGpxTime(point.time, time, sizeof(time));
        builder.addf("<time>%s</time>", time);
    }
    char name[256];
    escapeXml(point.name, name, sizeof(name));
    builder.addf("<name>%s</name>", name);
}

} // namespace

void formatGpxTime(int64_t seconds, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return;
    }
    int64_t days = seconds / 86400;
    int64_t remainder = seconds % 86400;
    if (remainder < 0) {
        remainder += 86400;
        days--;
    }
    int year = 1970;
    unsigned month = 1;
    unsigned day = 1;
    civilFromDays(days, year, month, day);
    const unsigned hour = static_cast<unsigned>(remainder / 3600);
    const unsigned minute = static_cast<unsigned>((remainder % 3600) / 60);
    const unsigned second = static_cast<unsigned>(remainder % 60);
    snprintf(out, size, "%04d-%02u-%02uT%02u:%02u:%02uZ", year, month, day, hour, minute, second);
}

void escapeXml(const char* text, char* out, size_t size) {
    if (out == nullptr || size == 0) {
        return;
    }
    size_t used = 0;
    for (const char* p = text; *p != '\0'; p++) {
        const char* replacement = nullptr;
        switch (*p) {
        case '&':
            replacement = "&amp;";
            break;
        case '<':
            replacement = "&lt;";
            break;
        case '>':
            replacement = "&gt;";
            break;
        case '"':
            replacement = "&quot;";
            break;
        case '\'':
            replacement = "&apos;";
            break;
        default:
            break;
        }
        if (replacement == nullptr) {
            if (used + 1 >= size) {
                break;
            }
            out[used++] = *p;
        } else {
            const size_t length = strlen(replacement);
            if (used + length >= size) {
                break;
            }
            memcpy(out + used, replacement, length);
            used += length;
        }
    }
    out[used] = '\0';
}

int gpxHeader(char* out, size_t size, const char* version) {
    TextBuilder builder(out, size);
    char escaped[128];
    escapeXml(version, escaped, sizeof(escaped));
    builder.add("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    builder.addf(
        "<gpx version=\"1.1\" creator=\"TactilityMaps %s\" xmlns=\"http://www.topografix.com/GPX/1/1\">\n",
        escaped);
    return builder.result();
}

int gpxFooter(char* out, size_t size) {
    TextBuilder builder(out, size);
    builder.add("</gpx>\n");
    return builder.result();
}

int gpxWaypoint(char* out, size_t size, const GpxPoint& point) {
    TextBuilder builder(out, size);
    addPointBody(builder, point, "wpt");
    builder.add("</wpt>\n");
    return builder.result();
}

int gpxTrackStart(char* out, size_t size, const char* name) {
    TextBuilder builder(out, size);
    char escaped[256];
    escapeXml(name, escaped, sizeof(escaped));
    builder.addf("<trk><name>%s</name><trkseg>\n", escaped);
    return builder.result();
}

int gpxTrackPoint(char* out, size_t size, const GpxPoint& point) {
    TextBuilder builder(out, size);
    addPointBody(builder, point, "trkpt");
    builder.add("</trkpt>\n");
    return builder.result();
}

int gpxTrackEnd(char* out, size_t size) {
    TextBuilder builder(out, size);
    builder.add("</trkseg></trk>\n");
    return builder.result();
}

} // namespace maps
