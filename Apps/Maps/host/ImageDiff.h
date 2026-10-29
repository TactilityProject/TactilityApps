// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Compares two RGB565 images, for checking that prepared tiles render like their originals.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace maps {

struct ImageDiff {
    /** Mean absolute difference per colour channel, scaled to 0-255. */
    double meanDifference;
    /** Share of pixels where a channel differs by more than 64 of 255. */
    double strongShare;
};

inline ImageDiff compareImages(const uint16_t* a, const uint16_t* b, size_t pixels) {
    double sum = 0.0;
    size_t strong = 0;
    for (size_t i = 0; i < pixels; i++) {
        const int channels[3][2] = {
            { (a[i] >> 11) * 255 / 31, (b[i] >> 11) * 255 / 31 },
            { ((a[i] >> 5) & 63) * 255 / 63, ((b[i] >> 5) & 63) * 255 / 63 },
            { (a[i] & 31) * 255 / 31, (b[i] & 31) * 255 / 31 },
        };
        bool isStrong = false;
        for (const auto& channel : channels) {
            const int difference = std::abs(channel[0] - channel[1]);
            sum += difference;
            isStrong = isStrong || difference > 64;
        }
        if (isStrong) {
            strong++;
        }
    }
    return { sum / (3.0 * static_cast<double>(pixels)), static_cast<double>(strong) / static_cast<double>(pixels) };
}

} // namespace maps
