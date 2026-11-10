// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <cstdio>

namespace maps {

/** Writes an RGB565 frame as a 24-bit BMP file, for screenshots while LVGL is stopped. */
bool saveBmp(const char* path, const uint16_t* pixels, int width, int height);

/**
 * Plays <root>/debug.txt when it exists, so the app can be tested without touching the device.
 * Lines: "key <char|esc|enter|bs>", "wait <ms>", "shot <name>", "goto <lat> <lon> <zoom>", "idle" (waits until the view is complete), "exit".
 * Touch, fed to the same gesture code as the touch screen (the script waits until a touch has ended):
 * "tap <x> <y>", "hold <x> <y> <ms>", "drag <x1> <y1> <x2> <y2> <ms>", "pinch <cx> <cy> <d1> <d2> <ms>" (finger distance
 * from d1 to d2), "twist <cx> <cy> <r> <a1> <a2> <ms>" (two fingers at distance 2r turning from a1 to a2 degrees).
 * "touchonly": from then on the app acts as a board with only a touch screen (keyboards and pointers ignored).
 * "squeeze <KB>": PSRAM is held in 64 KB pieces until no free block is that large, to test drawing with little memory.
 */
class DebugScript {
public:
    enum class Action {
        None,
        Key,
        Shot,
        Goto,
        WaitIdle,
        Exit,
        Touch,
        TouchOnly,
        /** "squeeze <KB>": PSRAM is held in pieces until no free block is that large, as after a long session. */
        Squeeze
    };

    enum class TouchKind : uint8_t { Tap, Hold, Drag, Pinch, Twist };

    struct Step {
        Action action;
        uint32_t key;
        char name[32];
        double lat;
        double lon;
        int zoom;
        TouchKind touch;
        /** Tap, hold: x, y. Drag: x1, y1, x2, y2. Pinch: cx, cy, d1, d2. Twist: cx, cy, r, a1, a2. */
        float values[5];
        uint32_t durationMs;
    };

    ~DebugScript();
    /** Reads the whole script into memory. Call it on the I/O task. */
    bool open(const char* root);
    bool isActive() const { return text != nullptr; }
    /** Returns the next step once its wait is over, or Action::None. */
    Step next(uint32_t now);

private:
    char* text = nullptr;
    size_t position = 0;
    uint32_t resumeAt = 0;
};

} // namespace maps
