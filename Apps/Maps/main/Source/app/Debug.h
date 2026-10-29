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
 */
class DebugScript {
public:
    enum class Action {
        None,
        Key,
        Shot,
        Goto,
        WaitIdle,
        Exit
    };

    struct Step {
        Action action;
        uint32_t key;
        char name[32];
        double lat;
        double lon;
        int zoom;
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
