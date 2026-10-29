// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

struct TouchSample {
    uint8_t count;
    int x[2];
    int y[2];
};

enum class GestureType : uint8_t {
    /** A finger went up without moving: reported at once, for buttons. */
    Tap,
    /** A tap with no second tap after it. */
    SingleTap,
    DoubleTap,
    LongPress,
    /** The finger moved by dx, dy since the last report. */
    Drag,
    PinchStart,
    /** scale is the finger distance against the start, around centerX, centerY; startX, startY is the first centre. */
    PinchMove,
    PinchEnd
};

struct Gesture {
    GestureType type;
    int x;
    int y;
    int dx;
    int dy;
    float scale;
    int startX;
    int startY;
};

/**
 * Turns touch readings into gestures. A tap is reported at once as Tap, so buttons respond without delay, and
 * again as SingleTap or DoubleTap once it is clear whether a second tap follows. Dragging, pinching and long
 * presses never count as taps.
 */
class Gestures {
public:
    static constexpr uint32_t DOUBLE_TAP_MS = 300;
    static constexpr uint32_t LONG_PRESS_MS = 600;
    static constexpr int TAP_SLOP = 10;
    static constexpr int DOUBLE_TAP_DISTANCE = 30;
    static constexpr int MAX_GESTURES = 4;

    /** Feeds one reading. Returns the number of gestures written to out, at most MAX_GESTURES. */
    int update(const TouchSample& sample, uint32_t now, Gesture* out);

    /** The last Tap was used by a button: it becomes neither a SingleTap nor half of a DoubleTap. */
    void consumeTap() { pendingTap = false; }

    bool isTouching() const { return fingers > 0; }
    bool isPinching() const { return pinching; }

private:
    void emit(Gesture* out, int& count, const Gesture& gesture);

    uint8_t fingers = 0;
    int startX = 0;
    int startY = 0;
    int lastX = 0;
    int lastY = 0;
    uint32_t downAt = 0;
    bool moved = false;
    bool longPressed = false;
    bool pinching = false;
    /** After a pinch, the remaining finger does nothing until all fingers are up. */
    bool spent = false;
    float pinchStartDistance = 0.0f;
    int pinchStartX = 0;
    int pinchStartY = 0;
    float pinchScale = 1.0f;
    int pinchX = 0;
    int pinchY = 0;
    bool pendingTap = false;
    uint32_t pendingAt = 0;
    int pendingX = 0;
    int pendingY = 0;
};

} // namespace maps
