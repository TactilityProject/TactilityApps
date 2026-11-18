// SPDX-License-Identifier: GPL-3.0-or-later
#include "Haptic.h"

#include "../platform/Platform.h"

#include <tactility/device.h>
#include <tactility/drivers/haptic.h>

namespace maps {

namespace {

// DRV260x ROM library 1 (ERM), effect 1 "strong click 100 %"; a slot value with the top bit set waits (10 ms units)
constexpr uint8_t LIBRARY = 1;
constexpr uint8_t STRONG_CLICK = 1;
constexpr uint8_t WAIT_200_MS = 0x80 | 20;

bool takeFirst(Device* found, void* context) {
    auto** device = static_cast<Device**>(context);
    if (*device != nullptr || !device_is_ready(found)) {
        return true;
    }
    if (device_get(found) == ERROR_NONE) {
        *device = found;
        return false;
    }
    return true;
}

} // namespace

Haptic::~Haptic() {
    end();
}

void Haptic::begin() {
    if (device == nullptr) {
        device_for_each_of_type(&HAPTIC_TYPE, &device, takeFirst);
        if (device != nullptr) {
            logInfo("Haptic: vibration motor found, turns are felt");
        }
    }
}

void Haptic::end() {
    if (device != nullptr) {
        device_put(device);
        device = nullptr;
    }
}

void Haptic::pulse(int count) {
    if (device == nullptr) {
        return;
    }
    const uint8_t sequence[4] = { STRONG_CLICK, count > 1 ? WAIT_200_MS : static_cast<uint8_t>(0), count > 1 ? STRONG_CLICK : static_cast<uint8_t>(0), 0 };
    bool ok = haptic_select_library(device, LIBRARY) == ERROR_NONE;
    for (uint8_t slot = 0; slot < 4 && ok; slot++) {
        ok = haptic_set_waveform(device, slot, sequence[slot]) == ERROR_NONE;
    }
    if (!ok || haptic_start_playback(device) != ERROR_NONE) {
        logError("Haptic: the motor did not take the pulse");
    }
}

} // namespace maps
