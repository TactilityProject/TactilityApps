// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

/** One position fix. Values the provider doesn't know are NAN (floats) or 0. */
struct PositionFix {
    double latitude;
    double longitude;
    /** Radius of 68 % confidence in metres. */
    float accuracy;
    /** Direction of travel in degrees clockwise from north. */
    float course;
    /** Metres per second. */
    float speed;
    /** Milliseconds since the Unix epoch. */
    int64_t timeMs;
};

enum class PositionRead : uint8_t {
    Fix,
    /** Location services are on, but no fix was reported yet. */
    NoFix,
    /** Location services are off in the system settings. */
    Off
};

/**
 * Where the position comes from. On the device this is Tactility's location module and nothing else,
 * on the PC a fake that tests feed with fixes. The app only reads, it never changes location settings.
 */
class PositionSource {
public:
    virtual ~PositionSource() = default;
    virtual PositionRead read(PositionFix& fix) = 0;
};

} // namespace maps
