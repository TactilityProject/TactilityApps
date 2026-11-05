// SPDX-License-Identifier: GPL-3.0-or-later
#include "GnssTypes.h"

namespace gpsviewer {

GnssSystem systemFromTalker(const char* talker) {
    if (talker == nullptr || talker[0] == '\0') {
        return GnssSystem::Unknown;
    }
    // No tables of string pointers: the app loader leaves pointers in data tables unrelocated
    const char first = talker[0];
    const char second = talker[1];
    if (first == 'G') {
        switch (second) {
            case 'P': return GnssSystem::Gps;
            case 'L': return GnssSystem::Glonass;
            case 'A': return GnssSystem::Galileo;
            case 'B': return GnssSystem::Beidou;
            case 'Q': return GnssSystem::Qzss;
            case 'I': return GnssSystem::Navic;
            default: return GnssSystem::Unknown;
        }
    }
    if (first == 'B' && second == 'D') return GnssSystem::Beidou;
    if (first == 'Q' && second == 'Z') return GnssSystem::Qzss;
    return GnssSystem::Unknown;
}

GnssSystem systemFromId(int id) {
    if (id >= 1 && id <= 32) return GnssSystem::Gps;
    if ((id >= 33 && id <= 64) || (id >= 120 && id <= 158)) return GnssSystem::Sbas;
    if (id >= 65 && id <= 96) return GnssSystem::Glonass;
    if (id >= 193 && id <= 202) return GnssSystem::Qzss;
    return GnssSystem::Unknown;
}

GnssSystem systemOfSatellite(const char* talker, int id) {
    const GnssSystem fromTalker = systemFromTalker(talker);
    if (fromTalker == GnssSystem::Unknown) {
        return systemFromId(id);
    }
    if (fromTalker == GnssSystem::Gps) {
        const GnssSystem fromId = systemFromId(id);
        if (fromId == GnssSystem::Sbas || fromId == GnssSystem::Qzss) {
            return fromId;
        }
    }
    return fromTalker;
}

char systemLetter(GnssSystem system) {
    switch (system) {
        case GnssSystem::Gps: return 'G';
        case GnssSystem::Glonass: return 'R';
        case GnssSystem::Galileo: return 'E';
        case GnssSystem::Beidou: return 'C';
        case GnssSystem::Qzss: return 'J';
        case GnssSystem::Sbas: return 'S';
        case GnssSystem::Navic: return 'I';
        default: return '?';
    }
}

const char* systemName(GnssSystem system) {
    switch (system) {
        case GnssSystem::Gps: return "GPS";
        case GnssSystem::Glonass: return "GLONASS";
        case GnssSystem::Galileo: return "Galileo";
        case GnssSystem::Beidou: return "BeiDou";
        case GnssSystem::Qzss: return "QZSS";
        case GnssSystem::Sbas: return "SBAS";
        case GnssSystem::Navic: return "NavIC";
        default: return "Unknown";
    }
}

} // namespace gpsviewer
