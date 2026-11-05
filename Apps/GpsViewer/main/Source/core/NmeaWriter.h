// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GnssTypes.h"

#include <cstddef>

namespace gpsviewer {

// Formats an event back into one NMEA 0183 line, "$<talker><type>,...*XX\r\n", so that
// parseNmeaLine() gives the same event again. An empty talker is written as "GN".
// Returns the length written without the NUL, or 0 when it doesn't fit in out.
size_t formatNmeaLine(const GnssEvent& event, char* out, size_t outSize);

} // namespace gpsviewer
