// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GnssTypes.h"
#include "SatelliteDb.h"

#include <cstdint>

namespace gpsviewer {

enum class FixType : uint8_t {
    None,
    Fix2d,
    Fix3d,
};

struct UtcDateTime {
    bool valid;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    int second;
};

// Everything the screens show, copied out of GnssModel in one go
struct GnssSnapshot {
    FixType fix;
    int satellitesUsed;
    int satellitesInView;
    // NAN when unknown
    float pdop;
    float hdop;
    float vdop;
    bool hasTtff;
    uint32_t ttffMs;
    uint32_t listeningMs;
    bool hasPosition;
    double latitude;
    double longitude;
    bool hasAltitude;
    float altitudeMeters;
    bool hasSpeed;
    float speedMps;
    float maxSpeedMps;
    float averageSpeedMps;
    // NAN while slower than COURSE_MIN_SPEED_MPS
    float courseDegrees;
    UtcDateTime utc;
    int satelliteCount;
    Satellite satellites[SatelliteDb::MAX_SATELLITES];
};

constexpr float COURSE_MIN_SPEED_MPS = 1.0f;
constexpr float METERS_PER_SECOND_PER_KNOT = 0.514444f;

class GnssModel {
public:
    void reset(uint32_t nowMs);
    void onEvent(const GnssEvent& event, uint32_t nowMs);
    void resetSpeedStats();
    void snapshot(GnssSnapshot& out, uint32_t nowMs);
    int satellitesInView() const { return satellites.count(); }
    const UtcDateTime& utc() const { return utcTime; }

private:
    void onRmc(const minmea_sentence_rmc& rmc, uint32_t nowMs);
    void onGga(const minmea_sentence_gga& gga, uint32_t nowMs);
    void onGsa(const minmea_sentence_gsa& gsa, uint32_t nowMs);
    // The fix shown at nowMs: a fresh GSA decides, a fresh GGA without a fix overrides it
    FixType currentFix(uint32_t nowMs) const;
    void updateFixState(uint32_t nowMs);

    SatelliteDb satellites;
    uint32_t startMs = 0;
    FixType gsaFix = FixType::None;
    uint32_t gsaAtMs = 0;
    bool ggaFix = false;
    int ggaSatellites = 0;
    uint32_t ggaAtMs = 0;
    bool rmcValid = false;
    uint32_t rmcAtMs = 0;
    float pdop = 0;
    float gsaHdop = 0;
    float ggaHdop = 0;
    float vdop = 0;
    bool hasTtff = false;
    uint32_t ttffMs = 0;
    bool hasPosition = false;
    double latitude = 0;
    double longitude = 0;
    // Altitude comes from GGA, speed and course from RMC. Each expires with its sentence.
    bool hasAltitude = false;
    float altitudeMeters = 0;
    bool hasSpeed = false;
    float speedMps = 0;
    float courseDegrees = 0;
    float maxSpeedMps = 0;
    double speedSum = 0;
    uint32_t speedSamples = 0;
    UtcDateTime utcTime {};
};

} // namespace gpsviewer
