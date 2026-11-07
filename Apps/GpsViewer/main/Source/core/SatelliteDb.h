// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GnssTypes.h"

#include <cstdint>

namespace gpsviewer {

struct Satellite {
    GnssSystem system;
    // Index of the GSV talker group that reported it, see SatelliteDb
    uint8_t group;
    int16_t id;
    // Degrees, -1 when the receiver gave no position
    int16_t elevation;
    int16_t azimuth;
    // dB-Hz, 0 when not tracked
    int16_t snr;
    bool used;
    // The GSA sentence that marked it used: its talker's system, or the system picked for a combined talker
    GnssSystem usedBy;
    uint32_t lastSeenMs;
    uint32_t usedAtMs;
};

// Satellites in view, assembled from GSV sets and marked used from GSA sentences
class SatelliteDb {
public:
    static constexpr int MAX_SATELLITES = 72;
    static constexpr int MAX_GROUPS = 8;
    static constexpr uint32_t EXPIRY_MS = 10000;
    static constexpr uint32_t USED_EXPIRY_MS = 3000;

    void clear();
    void onGsv(const char* talker, const minmea_sentence_gsv& gsv, uint32_t nowMs);
    void onGsa(const char* talker, const minmea_sentence_gsa& gsa, uint32_t nowMs);
    // Drops satellites not seen for EXPIRY_MS and used flags older than USED_EXPIRY_MS
    void expire(uint32_t nowMs);

    int count() const { return satelliteCount; }
    const Satellite& at(int index) const { return satellites[index]; }
    int usedCount() const;

private:
    struct PendingSet {
        char talker[3];
        int totalMessages;
        int nextMessage;
        int count;
        Satellite satellites[MAX_SATELLITES];
        // First commit of the current epoch. Later sets within SAME_EPOCH_MS of it are merged into it.
        uint32_t epochStartMs;
        bool committed;
    };

    int groupIndex(const char* talker);
    void commit(int group, uint32_t nowMs);
    int find(GnssSystem system, int id) const;
    void markUsed(GnssSystem system, const int* ids, int idCount, uint32_t nowMs);
    void clearUsed();
    GnssSystem guessGsaSystem(const int* ids, int idCount) const;

    Satellite satellites[MAX_SATELLITES] {};
    int satelliteCount = 0;
    PendingSet groups[MAX_GROUPS] {};
    int groupCount = 0;
    uint32_t combinedGsaAtMs = 0;
    uint32_t combinedGsaSystems = 0;
};

} // namespace gpsviewer
