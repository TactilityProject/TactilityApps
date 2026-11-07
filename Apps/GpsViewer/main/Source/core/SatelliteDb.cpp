// SPDX-License-Identifier: GPL-3.0-or-later
#include "SatelliteDb.h"

#include <cstring>

namespace gpsviewer {

namespace {

// GSV sets of the same talker within this time of the epoch's first set belong to that epoch.
// Receivers send one set per signal band (L1, L5) right after each other, so the sets are merged.
// The window is counted from the first set, so a receiver sending faster than this still starts new epochs.
constexpr uint32_t SAME_EPOCH_MS = 500;
// Combined (GN) GSA sentences closer together than this belong to one epoch
constexpr uint32_t GSA_EPOCH_MS = 300;

bool olderThan(uint32_t nowMs, uint32_t thenMs, uint32_t ageMs) {
    return static_cast<uint32_t>(nowMs - thenMs) > ageMs;
}

uint32_t systemBit(GnssSystem system) {
    return 1U << static_cast<uint32_t>(system);
}

// Maps an ID from a GSA sentence to its satellite system, once the sentence's system is known
GnssSystem usedIdSystem(GnssSystem gsaSystem, int id) {
    if (gsaSystem == GnssSystem::Gps) {
        return systemOfSatellite("GP", id);
    }
    if (gsaSystem == GnssSystem::Unknown) {
        return systemFromId(id);
    }
    return gsaSystem;
}

} // namespace

void SatelliteDb::clear() {
    satelliteCount = 0;
    groupCount = 0;
    combinedGsaAtMs = 0;
    combinedGsaSystems = 0;
}

int SatelliteDb::groupIndex(const char* talker) {
    for (int i = 0; i < groupCount; i++) {
        if (strncmp(groups[i].talker, talker, 2) == 0) {
            return i;
        }
    }
    if (groupCount == MAX_GROUPS) {
        return -1;
    }
    PendingSet& set = groups[groupCount];
    set = {};
    strncpy(set.talker, talker, 2);
    set.talker[2] = '\0';
    return groupCount++;
}

int SatelliteDb::find(GnssSystem system, int id) const {
    for (int i = 0; i < satelliteCount; i++) {
        if (satellites[i].system == system && satellites[i].id == id) {
            return i;
        }
    }
    return -1;
}

void SatelliteDb::onGsv(const char* talker, const minmea_sentence_gsv& gsv, uint32_t nowMs) {
    const int group = groupIndex(talker);
    if (group < 0) {
        return;
    }
    PendingSet& set = groups[group];
    if (gsv.msg_nr == 1) {
        set.count = 0;
        set.totalMessages = gsv.total_msgs;
        set.nextMessage = 1;
    }
    if (gsv.msg_nr != set.nextMessage || gsv.total_msgs != set.totalMessages || gsv.total_msgs < 1) {
        set.nextMessage = 0;
        return;
    }
    // NMEA 4.10 adds a signal ID after the satellites, which minmea reads as the next satellite's
    // number in a last message with fewer than four satellites. Entries beyond total_sats are dropped.
    for (int i = 0; i < 4; i++) {
        const minmea_sat_info& info = gsv.sats[i];
        const int position = (gsv.msg_nr - 1) * 4 + i;
        if (info.nr <= 0 || set.count == MAX_SATELLITES || (gsv.total_sats > 0 && position >= gsv.total_sats)) {
            continue;
        }
        Satellite& satellite = set.satellites[set.count++];
        satellite = {};
        satellite.system = systemOfSatellite(talker, info.nr);
        satellite.group = static_cast<uint8_t>(group);
        satellite.id = static_cast<int16_t>(info.nr);
        // minmea gives 0 for empty fields, and receivers leave both empty for satellites without a position
        const bool hasPosition = info.elevation != 0 || info.azimuth != 0;
        satellite.elevation = static_cast<int16_t>(hasPosition ? info.elevation : -1);
        satellite.azimuth = static_cast<int16_t>(hasPosition ? info.azimuth : -1);
        satellite.snr = static_cast<int16_t>(info.snr > 0 ? info.snr : 0);
        satellite.lastSeenMs = nowMs;
    }
    if (gsv.msg_nr == gsv.total_msgs) {
        commit(group, nowMs);
        set.nextMessage = 0;
    } else {
        set.nextMessage++;
    }
}

void SatelliteDb::commit(int group, uint32_t nowMs) {
    PendingSet& set = groups[group];
    const bool merge = set.committed && !olderThan(nowMs, set.epochStartMs, SAME_EPOCH_MS);

    if (!merge) {
        int kept = 0;
        for (int i = 0; i < satelliteCount; i++) {
            if (satellites[i].group != group) {
                satellites[kept++] = satellites[i];
                continue;
            }
            // Keep the used flag of satellites that are in the new set
            for (int j = 0; j < set.count; j++) {
                Satellite& fresh = set.satellites[j];
                if (fresh.system == satellites[i].system && fresh.id == satellites[i].id) {
                    fresh.used = satellites[i].used;
                    fresh.usedBy = satellites[i].usedBy;
                    fresh.usedAtMs = satellites[i].usedAtMs;
                }
            }
        }
        satelliteCount = kept;
    }

    for (int j = 0; j < set.count; j++) {
        const Satellite& fresh = set.satellites[j];
        const int index = find(fresh.system, fresh.id);
        if (index < 0) {
            if (satelliteCount < MAX_SATELLITES) {
                satellites[satelliteCount++] = fresh;
            }
            continue;
        }
        Satellite& existing = satellites[index];
        if (!merge || fresh.snr > existing.snr) {
            existing.snr = fresh.snr;
        }
        if (fresh.elevation >= 0) {
            existing.elevation = fresh.elevation;
            existing.azimuth = fresh.azimuth;
        }
        existing.lastSeenMs = nowMs;
    }

    if (!merge) {
        set.epochStartMs = nowMs;
    }
    set.committed = true;
}

// Combined GSA sentences don't say their system in minmea's fields. Receivers send one per system
// per epoch, so each system is picked once per epoch: the one whose satellites in view contain
// most of the used IDs. Ties go to the first system in enum order. Without a match the ID ranges decide.
GnssSystem SatelliteDb::guessGsaSystem(const int* ids, int idCount) const {
    GnssSystem best = GnssSystem::Unknown;
    int bestScore = 0;
    for (int s = 1; s < GNSS_SYSTEM_COUNT; s++) {
        const auto system = static_cast<GnssSystem>(s);
        if (system == GnssSystem::Sbas || (combinedGsaSystems & systemBit(system)) != 0) {
            continue;
        }
        int score = 0;
        for (int i = 0; i < idCount; i++) {
            if (find(usedIdSystem(system, ids[i]), ids[i]) >= 0) {
                score++;
            }
        }
        if (score > bestScore) {
            best = system;
            bestScore = score;
        }
    }
    return best;
}

// A GSA sentence replaces the used flags it set last time, so a GP sentence also clears the
// SBAS and QZSS satellites it listed before, and leaves those of a QZ or GN sentence alone
void SatelliteDb::markUsed(GnssSystem gsaSystem, const int* ids, int idCount, uint32_t nowMs) {
    if (gsaSystem != GnssSystem::Unknown) {
        for (int i = 0; i < satelliteCount; i++) {
            if (satellites[i].used && satellites[i].usedBy == gsaSystem) {
                satellites[i].used = false;
            }
        }
    }
    for (int i = 0; i < idCount; i++) {
        const int index = find(usedIdSystem(gsaSystem, ids[i]), ids[i]);
        if (index >= 0) {
            satellites[index].used = true;
            satellites[index].usedBy = gsaSystem;
            satellites[index].usedAtMs = nowMs;
        }
    }
}

void SatelliteDb::clearUsed() {
    for (int i = 0; i < satelliteCount; i++) {
        satellites[i].used = false;
    }
}

// Used IDs belong to the talker's system when the talker names one. A GP talker can also list
// SBAS (33..64, 120..158) and QZSS (193..202) IDs. Combined talkers are resolved by guessGsaSystem().
void SatelliteDb::onGsa(const char* talker, const minmea_sentence_gsa& gsa, uint32_t nowMs) {
    int ids[12];
    int idCount = 0;
    for (int id : gsa.sats) {
        if (id > 0) {
            ids[idCount++] = id;
        }
    }

    GnssSystem system = systemFromTalker(talker);
    if (system == GnssSystem::Unknown) {
        if (idCount == 0) {
            // An empty combined sentence without a fix: no satellite is used. With a fix it's
            // only a system without used satellites, and its system can't be told.
            if (gsa.fix_type != MINMEA_GPGSA_FIX_2D && gsa.fix_type != MINMEA_GPGSA_FIX_3D) {
                clearUsed();
            }
            return;
        }
        if (combinedGsaAtMs == 0 || olderThan(nowMs, combinedGsaAtMs, GSA_EPOCH_MS)) {
            combinedGsaSystems = 0;
        }
        combinedGsaAtMs = nowMs;
        system = guessGsaSystem(ids, idCount);
        if (system != GnssSystem::Unknown) {
            combinedGsaSystems |= systemBit(system);
        }
    }
    markUsed(system, ids, idCount, nowMs);
}

void SatelliteDb::expire(uint32_t nowMs) {
    int kept = 0;
    for (int i = 0; i < satelliteCount; i++) {
        Satellite& satellite = satellites[i];
        if (olderThan(nowMs, satellite.lastSeenMs, EXPIRY_MS)) {
            continue;
        }
        if (satellite.used && olderThan(nowMs, satellite.usedAtMs, USED_EXPIRY_MS)) {
            satellite.used = false;
        }
        satellites[kept++] = satellite;
    }
    satelliteCount = kept;
}

int SatelliteDb::usedCount() const {
    int used = 0;
    for (int i = 0; i < satelliteCount; i++) {
        if (satellites[i].used) {
            used++;
        }
    }
    return used;
}

} // namespace gpsviewer
