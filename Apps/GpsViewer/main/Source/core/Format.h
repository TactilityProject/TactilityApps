// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>

namespace gpsviewer {

enum class Units {
    Metric,
    Imperial,
    Nautical,
};

enum class CoordinateFormat {
    Degrees,
    DegreesMinutes,
    DegreesMinutesSeconds,
};

// Estimated horizontal accuracy in meters: HDOP times 5 m. NMEA gives no real accuracy.
float estimatedAccuracyMeters(float hdop);

// Writes e.g. "47.49801° N", "47° 29.881' N" or "47° 29' 52.8\" N" (° is UTF-8)
void formatCoordinate(double degrees, bool isLatitude, CoordinateFormat format, char* out, size_t outSize);

// Speed number without unit, e.g. "12.3", and the unit "km/h", "mph" or "kn"
void formatSpeed(float metersPerSecond, Units units, char* out, size_t outSize);
const char* speedUnit(Units units);

// Altitude and accuracy: "123 m" or "404 ft". Nautical uses meters.
void formatAltitude(float meters, Units units, char* out, size_t outSize);
void formatAccuracy(float meters, Units units, char* out, size_t outSize);

// "1:05" for 65 s, "1:02:03" from one hour up
void formatDuration(unsigned seconds, char* out, size_t outSize);

} // namespace gpsviewer
