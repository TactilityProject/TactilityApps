// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

struct Device;

namespace maps {

/**
 * The board's vibration motor (Tactility HAPTIC_TYPE, e.g. a DRV2605 on the T-Deck Max or T-LoRa Pager), if it has
 * one. Boards without a motor (the T-Deck) do nothing.
 */
class Haptic {
public:
    ~Haptic();
    /** Finds the first ready haptic device. */
    void begin();
    void end();
    bool available() const { return device != nullptr; }
    /** One short pulse (count 1) or two with a pause between (count 2). */
    void pulse(int count);

private:
    Device* device = nullptr;
};

} // namespace maps
