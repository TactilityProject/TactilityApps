// SPDX-License-Identifier: GPL-3.0-or-later
#include "GnssModel.h"

#include <cmath>

namespace gpsviewer {

namespace {

// A fix source that went quiet for this long no longer counts
constexpr uint32_t FIX_TIMEOUT_MS = 5000;

bool olderThan(uint32_t nowMs, uint32_t thenMs, uint32_t ageMs) {
    return static_cast<uint32_t>(nowMs - thenMs) > ageMs;
}

// A time of 0 means the sentence never came
bool fresh(uint32_t nowMs, uint32_t atMs) {
    return atMs != 0 && !olderThan(nowMs, atMs, FIX_TIMEOUT_MS);
}

// Event times are 0 only in the first millisecond of uptime, which then counts as never
uint32_t stamp(uint32_t nowMs) {
    return nowMs == 0 ? 1 : nowMs;
}

float toFloatOrNan(const minmea_float& value) {
    return value.scale == 0 ? NAN : minmea_tofloat(&value);
}

// minmea_tocoord() returns a float, which loses about half a meter
double toCoordinate(const minmea_float& value) {
    if (value.scale == 0) {
        return NAN;
    }
    const double raw = static_cast<double>(value.value) / value.scale;
    const int degrees = static_cast<int>(raw / 100);
    return degrees + (raw - degrees * 100) / 60.0;
}

} // namespace

void GnssModel::reset(uint32_t nowMs) {
    satellites.clear();
    startMs = nowMs;
    gsaFix = FixType::None;
    gsaAtMs = 0;
    ggaFix = false;
    ggaSatellites = 0;
    ggaAtMs = 0;
    rmcValid = false;
    rmcAtMs = 0;
    pdop = NAN;
    gsaHdop = NAN;
    ggaHdop = NAN;
    vdop = NAN;
    hasTtff = false;
    ttffMs = 0;
    hasPosition = false;
    hasAltitude = false;
    hasSpeed = false;
    maxSpeedMps = 0;
    speedSum = 0;
    speedSamples = 0;
    utcTime = {};
}

void GnssModel::onEvent(const GnssEvent& event, uint32_t nowMs) {
    switch (event.type) {
        case GnssEventType::Rmc:
            onRmc(event.data.rmc, nowMs);
            break;
        case GnssEventType::Gga:
            onGga(event.data.gga, nowMs);
            break;
        case GnssEventType::Gsv:
            satellites.onGsv(event.talker, event.data.gsv, nowMs);
            break;
        case GnssEventType::Gsa:
            satellites.onGsa(event.talker, event.data.gsa, nowMs);
            onGsa(event.data.gsa, nowMs);
            break;
    }
    updateFixState(nowMs);
}

void GnssModel::onRmc(const minmea_sentence_rmc& rmc, uint32_t nowMs) {
    rmcValid = rmc.valid;
    rmcAtMs = stamp(nowMs);
    if (rmc.date.year > 0 && rmc.time.hours >= 0) {
        utcTime.valid = true;
        utcTime.year = rmc.date.year < 100 ? 2000 + rmc.date.year : rmc.date.year;
        utcTime.month = rmc.date.month;
        utcTime.day = rmc.date.day;
        utcTime.hour = rmc.time.hours;
        utcTime.minute = rmc.time.minutes;
        utcTime.second = rmc.time.seconds;
    }
    if (!rmc.valid) {
        hasSpeed = false;
        return;
    }
    const double lat = toCoordinate(rmc.latitude);
    const double lon = toCoordinate(rmc.longitude);
    if (!std::isnan(lat) && !std::isnan(lon)) {
        hasPosition = true;
        latitude = lat;
        longitude = lon;
    }
    const float knots = toFloatOrNan(rmc.speed);
    if (std::isnan(knots)) {
        hasSpeed = false;
        return;
    }
    hasSpeed = true;
    speedMps = knots * METERS_PER_SECOND_PER_KNOT;
    courseDegrees = toFloatOrNan(rmc.course);
    if (speedMps > maxSpeedMps) {
        maxSpeedMps = speedMps;
    }
    speedSum += speedMps;
    speedSamples++;
}

void GnssModel::onGga(const minmea_sentence_gga& gga, uint32_t nowMs) {
    ggaFix = gga.fix_quality > 0;
    ggaSatellites = gga.satellites_tracked;
    ggaAtMs = stamp(nowMs);
    ggaHdop = ggaFix ? toFloatOrNan(gga.hdop) : NAN;
    if (!ggaFix) {
        hasAltitude = false;
        return;
    }
    const double lat = toCoordinate(gga.latitude);
    const double lon = toCoordinate(gga.longitude);
    if (!std::isnan(lat) && !std::isnan(lon)) {
        hasPosition = true;
        latitude = lat;
        longitude = lon;
    }
    const float altitude = toFloatOrNan(gga.altitude);
    hasAltitude = !std::isnan(altitude) && (gga.altitude_units == 'M' || gga.altitude_units == '\0');
    if (hasAltitude) {
        altitudeMeters = altitude;
    }
}

void GnssModel::onGsa(const minmea_sentence_gsa& gsa, uint32_t nowMs) {
    gsaAtMs = stamp(nowMs);
    gsaFix = gsa.fix_type == MINMEA_GPGSA_FIX_3D ? FixType::Fix3d
        : gsa.fix_type == MINMEA_GPGSA_FIX_2D ? FixType::Fix2d
        : FixType::None;
    pdop = toFloatOrNan(gsa.pdop);
    gsaHdop = toFloatOrNan(gsa.hdop);
    vdop = toFloatOrNan(gsa.vdop);
}

FixType GnssModel::currentFix(uint32_t nowMs) const {
    const bool gsaFresh = fresh(nowMs, gsaAtMs);
    const bool ggaFresh = fresh(nowMs, ggaAtMs);
    if (ggaFresh && !ggaFix) {
        return FixType::None;
    }
    if (gsaFresh) {
        return gsaFix;
    }
    return ggaFresh ? FixType::Fix3d : FixType::None;
}

// TTFF uses the same fix as the screens, plus a valid RMC for the position
void GnssModel::updateFixState(uint32_t nowMs) {
    if (hasTtff) {
        return;
    }
    if (currentFix(nowMs) != FixType::None && rmcValid && fresh(nowMs, rmcAtMs)) {
        hasTtff = true;
        ttffMs = nowMs - startMs;
    }
}

void GnssModel::resetSpeedStats() {
    maxSpeedMps = hasSpeed ? speedMps : 0;
    speedSum = 0;
    speedSamples = 0;
}

void GnssModel::snapshot(GnssSnapshot& out, uint32_t nowMs) {
    satellites.expire(nowMs);

    const bool gsaFresh = fresh(nowMs, gsaAtMs);
    const bool ggaFresh = fresh(nowMs, ggaAtMs);
    const bool rmcFresh = fresh(nowMs, rmcAtMs);
    const FixType fix = currentFix(nowMs);

    out.fix = fix;
    out.satellitesInView = satellites.count();
    out.satellitesUsed = satellites.usedCount();
    if (out.satellitesUsed == 0 && ggaFresh && ggaFix) {
        out.satellitesUsed = ggaSatellites;
    }
    out.pdop = gsaFresh ? pdop : NAN;
    // GSA gives all three DOP values of one solution, GGA's HDOP stands in without GSA
    out.hdop = gsaFresh ? gsaHdop : ggaFresh ? ggaHdop : NAN;
    out.vdop = gsaFresh ? vdop : NAN;
    out.hasTtff = hasTtff;
    out.ttffMs = ttffMs;
    out.listeningMs = nowMs - startMs;
    out.hasPosition = hasPosition;
    out.latitude = latitude;
    out.longitude = longitude;
    out.hasAltitude = hasAltitude && ggaFresh && fix == FixType::Fix3d;
    out.altitudeMeters = altitudeMeters;
    out.hasSpeed = hasSpeed && rmcFresh && fix != FixType::None;
    out.speedMps = speedMps;
    out.maxSpeedMps = maxSpeedMps;
    out.averageSpeedMps = speedSamples == 0 ? 0.0f : static_cast<float>(speedSum / speedSamples);
    out.courseDegrees = (out.hasSpeed && speedMps >= COURSE_MIN_SPEED_MPS) ? courseDegrees : NAN;
    out.utc = utcTime;
    out.satelliteCount = satellites.count();
    for (int i = 0; i < out.satelliteCount; i++) {
        out.satellites[i] = satellites.at(i);
    }
}

} // namespace gpsviewer
