// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <tactility/device.h>

#include <cstdint>

namespace maps {

struct KeyEvent {
    uint32_t key;
    bool pressed;
};

struct TouchState {
    uint8_t count;
    uint16_t x[2];
    uint16_t y[2];
};

/**
 * Reads keyboards (the T-Deck keyboard and a trackball in keyboard mode), mice (a trackball in mouse mode) and
 * the touch screen directly from their drivers while LVGL is stopped.
 */
class Input {
public:
    static constexpr int MAX_DEVICES = 4;

    ~Input();

    void begin();
    void end();

    /** Returns the number of key events written to events. */
    int readKeys(KeyEvent* events, int maxEvents);
    /** Sums the movement of all mice since the last call. Returns true while a mouse button is held. */
    bool readMouse(int32_t& dx, int32_t& dy);
    bool readTouch(TouchState& state);
    bool hasTouch() const { return touch != nullptr; }
    bool hasKeyboard() const { return keyboardCount > 0; }
    bool hasPointer() const { return mouseCount > 0; }

private:
    void rescan();

    Device* keyboards[MAX_DEVICES] {};
    int keyboardCount = 0;
    Device* mice[MAX_DEVICES] {};
    int mouseCount = 0;
    Device* touch = nullptr;
    uint32_t lastScanMs = 0;
};

} // namespace maps
