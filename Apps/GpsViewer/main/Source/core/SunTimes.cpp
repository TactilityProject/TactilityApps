// SPDX-License-Identifier: GPL-3.0-or-later
#include "SunTimes.h"

#include <cmath>

namespace gpsviewer {

namespace {

constexpr double PI_VALUE = 3.14159265358979323846;
constexpr double DEG = PI_VALUE / 180.0;

struct SolarValues {
    double declination; // radians
    double equationOfTime; // minutes
};

SolarValues solarAt(double julianDay) {
    double t = (julianDay - 2451545.0) / 36525.0;
    double l0 = std::fmod(280.46646 + t * (36000.76983 + t * 0.0003032), 360.0);
    double m = (357.52911 + t * (35999.05029 - 0.0001537 * t)) * DEG;
    double e = 0.016708634 - t * (0.000042037 + 0.0000001267 * t);
    double center = std::sin(m) * (1.914602 - t * (0.004817 + 0.000014 * t))
        + std::sin(2 * m) * (0.019993 - 0.000101 * t)
        + std::sin(3 * m) * 0.000289;
    double trueLong = l0 + center;
    double omega = (125.04 - 1934.136 * t) * DEG;
    double apparent = (trueLong - 0.00569 - 0.00478 * std::sin(omega)) * DEG;
    double seconds = 21.448 - t * (46.815 + t * (0.00059 - t * 0.001813));
    double meanObliq = 23.0 + (26.0 + seconds / 60.0) / 60.0;
    double obliq = (meanObliq + 0.00256 * std::cos(omega)) * DEG;
    double decl = std::asin(std::sin(obliq) * std::sin(apparent));
    double y = std::tan(obliq / 2.0);
    y *= y;
    double l0r = l0 * DEG;
    double eot = y * std::sin(2 * l0r) - 2 * e * std::sin(m)
        + 4 * e * y * std::sin(m) * std::cos(2 * l0r)
        - 0.5 * y * y * std::sin(4 * l0r) - 1.25 * e * e * std::sin(2 * m);
    return { decl, 4.0 * eot / DEG };
}

} // namespace

long long utcToEpoch(int year, int month, int day, int hour, int minute, int second) {
    long long y = year;
    y -= month <= 2 ? 1 : 0;
    long long era = (y >= 0 ? y : y - 399) / 400;
    long long yoe = y - era * 400;
    long long mp = (month + 9) % 12;
    long long doy = (153 * mp + 2) / 5 + day - 1;
    long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long long days = era * 146097 + doe - 719468;
    return days * 86400 + hour * 3600LL + minute * 60LL + second;
}

SunTimes computeSunTimes(int year, int month, int day, double latitude, double longitude) {
    double jdMidnight = (double)utcToEpoch(year, month, day, 0, 0, 0) / 86400.0 + 2440587.5;
    // Solar noon first guess, then evaluate again at the refined noon
    SolarValues first = solarAt(jdMidnight + 0.5 - longitude / 360.0);
    double noon = 720.0 - 4.0 * longitude - first.equationOfTime;
    SolarValues s = solarAt(jdMidnight + noon / 1440.0);

    double lat = latitude * DEG;
    double cosHa = std::cos(90.833 * DEG) / (std::cos(lat) * std::cos(s.declination))
        - std::tan(lat) * std::tan(s.declination);

    SunTimes out { SunDay::Normal, 0.0, 0.0 };
    if (cosHa > 1.0) {
        out.day = SunDay::PolarNight;
        return out;
    }
    if (cosHa < -1.0) {
        out.day = SunDay::PolarDay;
        return out;
    }
    double ha = std::acos(cosHa) / DEG;
    // Not wrapped into 0..1440, so values may be negative or above 1440
    out.sunriseUtcMinutes = 720.0 - 4.0 * (longitude + ha) - s.equationOfTime;
    out.sunsetUtcMinutes = 720.0 - 4.0 * (longitude - ha) - s.equationOfTime;
    return out;
}

} // namespace gpsviewer
