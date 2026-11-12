// SPDX-License-Identifier: GPL-3.0-or-later
#include "Track.h"

#include <cmath>

namespace maps {

namespace {

constexpr double EARTH_RADIUS = 6371008.8;
constexpr double DEGREES = 3.14159265358979323846 / 180.0;

} // namespace

double distanceMetres(double lat1, double lon1, double lat2, double lon2) {
    // Haversine, with the arc sine written through atan2, which is exported to apps
    const double dLat = (lat2 - lat1) * DEGREES;
    double dLon = (lon2 - lon1) * DEGREES;
    if (dLon > 3.14159265358979323846) {
        dLon -= 2.0 * 3.14159265358979323846;
    } else if (dLon < -3.14159265358979323846) {
        dLon += 2.0 * 3.14159265358979323846;
    }
    const double s = std::sin(dLat / 2.0);
    const double t = std::sin(dLon / 2.0);
    const double h = s * s + std::cos(lat1 * DEGREES) * std::cos(lat2 * DEGREES) * t * t;
    return 2.0 * EARTH_RADIUS * std::atan2(std::sqrt(h), std::sqrt(1.0 - h));
}

double bearingDegrees(double lat1, double lon1, double lat2, double lon2) {
    const double phi1 = lat1 * DEGREES;
    const double phi2 = lat2 * DEGREES;
    const double dLon = (lon2 - lon1) * DEGREES;
    const double y = std::sin(dLon) * std::cos(phi2);
    const double x = std::cos(phi1) * std::sin(phi2) - std::sin(phi1) * std::cos(phi2) * std::cos(dLon);
    const double bearing = std::atan2(y, x) / DEGREES;
    return bearing < 0.0 ? bearing + 360.0 : bearing;
}

bool TrackFilter::accept(const PositionFix& fix) {
    if (std::isnan(fix.latitude) || std::isnan(fix.longitude) || (!std::isnan(fix.accuracy) && fix.accuracy >= APPROXIMATE_METRES)) {
        return false;
    }
    if (!stored) {
        stored = true;
        last = fix;
        return true;
    }
    const double moved = distanceMetres(last.latitude, last.longitude, fix.latitude, fix.longitude);
    const double accuracy = std::isnan(fix.accuracy) ? 0.0 : static_cast<double>(fix.accuracy);
    const double step = accuracy > MIN_STEP_METRES ? accuracy : MIN_STEP_METRES;
    // Every 30 s while moving at all: a slow walk still gets points, standing still adds none
    const bool due = fix.timeMs - last.timeMs >= MOVING_INTERVAL_MS && moved > MIN_STEP_METRES / 2.0;
    if (moved <= step && !due) {
        return false;
    }
    totalMetres += moved;
    last = fix;
    return true;
}

void TrackFilter::reset() {
    stored = false;
    last = {};
    totalMetres = 0.0;
}

} // namespace maps
