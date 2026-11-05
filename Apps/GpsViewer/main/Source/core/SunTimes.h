// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace gpsviewer {

enum class SunDay {
    Normal,
    PolarDay,
    PolarNight,
};

struct SunTimes {
    SunDay day;
    // Minutes after 00:00 UTC of the given date, can be below 0 or above 1440
    double sunriseUtcMinutes;
    double sunsetUtcMinutes;
};

// NOAA solar calculator algorithm, with the sun 0.833° below the horizon at rise and set
SunTimes computeSunTimes(int year, int month, int day, double latitude, double longitude);

// Seconds since 1970-01-01 00:00 UTC
long long utcToEpoch(int year, int month, int day, int hour, int minute, int second);

} // namespace gpsviewer
