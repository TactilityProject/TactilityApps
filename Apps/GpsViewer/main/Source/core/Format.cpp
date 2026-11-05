// SPDX-License-Identifier: GPL-3.0-or-later

#include "Format.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace gpsviewer {

namespace {

// llround() is not exported to Tactility apps
long long lroundExported(double value) {
    return static_cast<long long>(std::round(value));
}

float quietNaN() {
    return std::numeric_limits<float>::quiet_NaN();
}

void writeDash(char* out, size_t outSize) {
    std::snprintf(out, outSize, "--");
}

double speedFactor(Units units) {
    switch (units) {
        case Units::Imperial:
            return 2.236936;
        case Units::Nautical:
            return 1.943844;
        case Units::Metric:
            break;
    }
    return 3.6;
}

double lengthFactor(Units units) {
    return units == Units::Imperial ? 3.28084 : 1.0;
}

// One decimal below the limit, whole numbers from it on. The check uses the rounded value.
void writeAdaptive(double value, double limit, const char* suffix, char* out, size_t outSize) {
    if (std::round(value * 10.0) < limit * 10.0) {
        std::snprintf(out, outSize, "%.1f%s", value, suffix);
    } else {
        std::snprintf(out, outSize, "%lld%s", static_cast<long long>(std::round(value)), suffix);
    }
}

} // namespace

float estimatedAccuracyMeters(float hdop) {
    if (std::isnan(hdop) || hdop <= 0.0f) {
        return quietNaN();
    }
    return hdop * 5.0f;
}

void formatCoordinate(double degrees, bool isLatitude, CoordinateFormat format, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    if (std::isnan(degrees)) {
        writeDash(out, outSize);
        return;
    }

    const bool negative = degrees < 0.0;
    const char hemisphere = isLatitude ? (negative ? 'S' : 'N') : (negative ? 'W' : 'E');
    const double absolute = std::fabs(degrees);

    switch (format) {
        case CoordinateFormat::Degrees:
            std::snprintf(out, outSize, "%.5f\xC2\xB0 %c", absolute, hemisphere);
            return;
        case CoordinateFormat::DegreesMinutes: {
            // Thousandths of a minute, rounded before splitting so carries propagate
            const long long units = lroundExported(absolute * 60000.0);
            const long long deg = units / 60000;
            const long long rest = units % 60000;
            std::snprintf(out, outSize, "%lld\xC2\xB0 %lld.%03lld' %c",
                          deg, rest / 1000, rest % 1000, hemisphere);
            return;
        }
        case CoordinateFormat::DegreesMinutesSeconds: {
            // Tenths of a second, rounded before splitting so carries propagate
            const long long tenths = lroundExported(absolute * 36000.0);
            const long long deg = tenths / 36000;
            const long long rest = tenths % 36000;
            const long long minutes = rest / 600;
            const long long rest2 = rest % 600;
            std::snprintf(out, outSize, "%lld\xC2\xB0 %lld' %lld.%lld\" %c",
                          deg, minutes, rest2 / 10, rest2 % 10, hemisphere);
            return;
        }
    }
}

void formatSpeed(float metersPerSecond, Units units, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    if (std::isnan(metersPerSecond)) {
        writeDash(out, outSize);
        return;
    }
    writeAdaptive(static_cast<double>(metersPerSecond) * speedFactor(units), 100.0, "", out, outSize);
}

const char* speedUnit(Units units) {
    switch (units) {
        case Units::Imperial:
            return "mph";
        case Units::Nautical:
            return "kn";
        case Units::Metric:
            break;
    }
    return "km/h";
}

void formatAltitude(float meters, Units units, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    if (std::isnan(meters)) {
        writeDash(out, outSize);
        return;
    }
    const double value = static_cast<double>(meters) * lengthFactor(units);
    const char* suffix = units == Units::Imperial ? " ft" : " m";
    std::snprintf(out, outSize, "%lld%s", static_cast<long long>(std::round(value)), suffix);
}

void formatAccuracy(float meters, Units units, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    if (std::isnan(meters)) {
        writeDash(out, outSize);
        return;
    }
    const double value = static_cast<double>(meters) * lengthFactor(units);
    const char* suffix = units == Units::Imperial ? " ft" : " m";
    writeAdaptive(value, 10.0, suffix, out, outSize);
}

void formatDuration(unsigned seconds, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    const unsigned hours = seconds / 3600U;
    const unsigned minutes = (seconds / 60U) % 60U;
    const unsigned secs = seconds % 60U;
    if (hours > 0) {
        std::snprintf(out, outSize, "%u:%02u:%02u", hours, minutes, secs);
    } else {
        std::snprintf(out, outSize, "%u:%02u", minutes, secs);
    }
}

} // namespace gpsviewer
