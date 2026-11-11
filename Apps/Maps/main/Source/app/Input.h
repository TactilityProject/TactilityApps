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
 *
 * A trackball driver replaces its child device when its mode changes: the keyboard one is stopped and removed, a
 * mouse one is added, or the other way round. The device lists are refreshed twice a second, devices that stopped
 * are let go at once (a held reference keeps the driver from destroying the old child), and new ones are taken.
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
    /**
     * A keyboard that types text, so key hints make sense. Keys are read from every keyboard device, but buttons
     * (button-control) and a trackball also register as one: only a started keyboard device that reports itself
     * present counts, as for Tactility's software keyboard.
     */
    bool hasKeyboard() const { return typingKeyboard && !touchOnly; }
    bool hasPointer() const { return mouseCount > 0 && !touchOnly; }
    /** Debug scripts ("touchonly"): act as a board with only a touch screen; keyboards and pointers are ignored. */
    void setTouchOnly(bool value) { touchOnly = value; }
    /** True once after a keyboard or mouse came or went, so the overlay can adapt. */
    bool takeDevicesChanged();

private:
    void rescan();
    void refreshIfDue();
    /** Lets go of devices that stopped. Returns how many. */
    static int dropStopped(Device** devices, int& count);

    Device* keyboards[MAX_DEVICES] {};
    bool touchOnly = false;
    bool typingKeyboard = false;
    int keyboardCount = 0;
    Device* mice[MAX_DEVICES] {};
    int mouseCount = 0;
    Device* touch = nullptr;
    uint32_t lastScanMs = 0;
    bool devicesChanged = false;
};

} // namespace maps
