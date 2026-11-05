// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <minmea.h>

#include <cstdint>

namespace gpsviewer {

enum class GnssSystem : uint8_t {
    Unknown,
    Gps,
    Glonass,
    Galileo,
    Beidou,
    Qzss,
    Sbas,
    Navic,
};

constexpr int GNSS_SYSTEM_COUNT = 8;

enum class GnssEventType : uint8_t {
    Rmc,
    Gga,
    Gsv,
    Gsa,
};

// The data of one parsed NMEA sentence, filled from a GpsEvent
struct GnssEvent {
    GnssEventType type;
    char talker[3];
    union {
        minmea_sentence_rmc rmc;
        minmea_sentence_gga gga;
        minmea_sentence_gsv gsv;
        minmea_sentence_gsa gsa;
    } data;
};

// "GP" Gps, "GL" Glonass, "GA" Galileo, "GB" and "BD" Beidou, "GQ" and "QZ" Qzss, "GI" Navic.
// "GN", empty and anything else give Unknown: the satellite ID decides then.
GnssSystem systemFromTalker(const char* talker);

// NMEA ID ranges: 1..32 GPS, 33..64 and 120..158 SBAS, 65..96 GLONASS, 193..202 QZSS. Others give Unknown.
GnssSystem systemFromId(int id);

// System of a satellite listed under a talker. A GP talker also carries SBAS and QZSS IDs.
GnssSystem systemOfSatellite(const char* talker, int id);

// G GPS, R GLONASS, E Galileo, C BeiDou, J QZSS, S SBAS, I NavIC, ? unknown
char systemLetter(GnssSystem system);

const char* systemName(GnssSystem system);

} // namespace gpsviewer
