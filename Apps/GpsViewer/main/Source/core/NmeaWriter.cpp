// SPDX-License-Identifier: GPL-3.0-or-later
#include "NmeaWriter.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace gpsviewer {

namespace {

struct Out {
    char* buf;
    size_t cap;
    size_t len;
    bool ok;
};

void put(Out& o, const char* fmt, ...) {
    if (!o.ok) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o.buf + o.len, o.cap - o.len, fmt, ap);
    va_end(ap);
    if (n < 0 || static_cast<size_t>(n) >= o.cap - o.len) {
        o.ok = false;
        return;
    }
    o.len += static_cast<size_t>(n);
}

// Writes value/scale with log10(scale) decimals, nothing for scale 0 (empty field).
// withSign false writes the absolute value because the hemisphere letter carries the sign.
void putFloat(Out& o, const minmea_float& f, int intDigits, bool withSign) {
    if (f.scale <= 0) {
        return;
    }
    int64_t v = f.value;
    const bool negative = v < 0;
    if (negative) {
        v = -v;
    }
    int decimals = 0;
    int64_t div = 1;
    for (int64_t s = f.scale; s > 1; s /= 10) {
        decimals++;
        div *= 10;
    }
    if (negative && withSign) {
        put(o, "-");
    }
    const long long ip = static_cast<long long>(v / div);
    if (decimals > 0) {
        put(o, "%0*lld.%0*lld", intDigits, ip, decimals, static_cast<long long>(v % div));
    } else {
        put(o, "%0*lld", intDigits, ip);
    }
}

void putCoord(Out& o, const minmea_float& f, int intDigits, char positive, char negative) {
    put(o, ",");
    putFloat(o, f, intDigits, false);
    put(o, ",");
    if (f.scale > 0) {
        put(o, "%c", f.value < 0 ? negative : positive);
    }
}

void putTime(Out& o, const minmea_time& t) {
    put(o, ",");
    if (t.hours < 0) {
        return;
    }
    put(o, "%02d%02d%02d", t.hours, t.minutes, t.seconds);
    int us = t.microseconds < 0 ? 0 : t.microseconds;
    int digits = 6;
    while (digits > 2 && us % 10 == 0) {
        us /= 10;
        digits--;
    }
    put(o, ".%0*d", digits, us);
}

void putDate(Out& o, const minmea_date& d) {
    put(o, ",");
    if (d.day < 0) {
        return;
    }
    put(o, "%02d%02d%02d", d.day, d.month, d.year);
}

void putChar(Out& o, char c) {
    put(o, ",");
    if (c != 0) {
        put(o, "%c", c);
    }
}

void putFloatField(Out& o, const minmea_float& f) {
    put(o, ",");
    putFloat(o, f, 1, true);
}

void putRmc(Out& o, const minmea_sentence_rmc& r) {
    putTime(o, r.time);
    put(o, r.valid ? ",A" : ",V");
    putCoord(o, r.latitude, 4, 'N', 'S');
    putCoord(o, r.longitude, 5, 'E', 'W');
    putFloatField(o, r.speed);
    putFloatField(o, r.course);
    putDate(o, r.date);
    putCoord(o, r.variation, 1, 'E', 'W');
}

void putGga(Out& o, const minmea_sentence_gga& g) {
    putTime(o, g.time);
    putCoord(o, g.latitude, 4, 'N', 'S');
    putCoord(o, g.longitude, 5, 'E', 'W');
    put(o, ",%d,%02d", g.fix_quality, g.satellites_tracked);
    putFloatField(o, g.hdop);
    putFloatField(o, g.altitude);
    putChar(o, g.altitude_units);
    putFloatField(o, g.height);
    putChar(o, g.height_units);
    putFloatField(o, g.dgps_age);
    put(o, ",");
}

void putGsa(Out& o, const minmea_sentence_gsa& a) {
    putChar(o, a.mode);
    put(o, ",%d", a.fix_type);
    for (int i = 0; i < 12; i++) {
        put(o, ",");
        if (a.sats[i] != 0) {
            put(o, "%02d", a.sats[i]);
        }
    }
    putFloatField(o, a.pdop);
    putFloatField(o, a.hdop);
    putFloatField(o, a.vdop);
}

void putGsv(Out& o, const minmea_sentence_gsv& v) {
    put(o, ",%d,%d,%d", v.total_msgs, v.msg_nr, v.total_sats);
    int last = -1;
    for (int i = 0; i < 4; i++) {
        const minmea_sat_info& s = v.sats[i];
        if (s.nr != 0 || s.elevation != 0 || s.azimuth != 0 || s.snr != 0) {
            last = i;
        }
    }
    for (int i = 0; i <= last; i++) {
        const minmea_sat_info& s = v.sats[i];
        put(o, ",");
        if (s.nr != 0) {
            put(o, "%02d", s.nr);
        }
        put(o, ",%02d,%03d,%02d", s.elevation, s.azimuth, s.snr);
    }
}

} // namespace

size_t formatNmeaLine(const GnssEvent& event, char* out, size_t outSize) {
    if (out == nullptr || outSize == 0) {
        return 0;
    }
    Out o = {out, outSize, 0, true};
    const bool hasTalker = event.talker[0] != '\0' && event.talker[1] != '\0';
    const char t0 = hasTalker ? event.talker[0] : 'G';
    const char t1 = hasTalker ? event.talker[1] : 'N';
    const char* name = "RMC";
    switch (event.type) {
        case GnssEventType::Rmc: name = "RMC"; break;
        case GnssEventType::Gga: name = "GGA"; break;
        case GnssEventType::Gsv: name = "GSV"; break;
        case GnssEventType::Gsa: name = "GSA"; break;
    }
    put(o, "$%c%c%s", t0, t1, name);
    switch (event.type) {
        case GnssEventType::Rmc: putRmc(o, event.data.rmc); break;
        case GnssEventType::Gga: putGga(o, event.data.gga); break;
        case GnssEventType::Gsv: putGsv(o, event.data.gsv); break;
        case GnssEventType::Gsa: putGsa(o, event.data.gsa); break;
    }
    if (!o.ok) {
        out[0] = '\0';
        return 0;
    }
    unsigned checksum = 0;
    for (size_t i = 1; i < o.len; i++) {
        checksum ^= static_cast<unsigned char>(out[i]);
    }
    put(o, "*%02X\r\n", checksum);
    if (!o.ok) {
        out[0] = '\0';
        return 0;
    }
    return o.len;
}

} // namespace gpsviewer
