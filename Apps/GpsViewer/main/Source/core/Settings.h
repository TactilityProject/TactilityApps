// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Format.h"

#include <cstddef>

namespace gpsviewer {

struct Settings {
    Units units = Units::Metric;
    CoordinateFormat coordinateFormat = CoordinateFormat::Degrees;
    bool nightMode = false;
};

// Text format, one "key=value" per line: units=metric|imperial|nautical, coordinates=dd|dm|dms,
// night=0|1. Unknown keys and bad values are ignored and keep the defaults.
Settings parseSettings(const char* text);
// Returns the number of characters written, without the NUL
size_t serializeSettings(const Settings& settings, char* out, size_t outSize);

} // namespace gpsviewer
