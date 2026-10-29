// SPDX-License-Identifier: GPL-3.0-or-later
#include "Gestures.h"

#include <cmath>
#include <cstdlib>

namespace maps {

void Gestures::emit(Gesture* out, int& count, const Gesture& gesture) {
    if (count < MAX_GESTURES) {
        out[count++] = gesture;
    }
}

int Gestures::update(const TouchSample& sample, uint32_t now, Gesture* out) {
    int count = 0;
    // A tap becomes a SingleTap once no second tap can follow
    if (pendingTap && sample.count == 0 && fingers == 0 && now - pendingAt >= DOUBLE_TAP_MS) {
        pendingTap = false;
        emit(out, count, { GestureType::SingleTap, pendingX, pendingY, 0, 0, 1.0f, pendingX, pendingY });
    }

    if (sample.count >= 2) {
        const float dx = static_cast<float>(sample.x[0] - sample.x[1]);
        const float dy = static_cast<float>(sample.y[0] - sample.y[1]);
        const float distance = std::sqrt(dx * dx + dy * dy);
        const int centerX = (sample.x[0] + sample.x[1]) / 2;
        const int centerY = (sample.y[0] + sample.y[1]) / 2;
        if (!pinching) {
            pinching = true;
            spent = true;
            pendingTap = false;
            pinchStartDistance = distance > 1.0f ? distance : 1.0f;
            pinchStartX = centerX;
            pinchStartY = centerY;
            pinchScale = 1.0f;
            emit(out, count, { GestureType::PinchStart, centerX, centerY, 0, 0, 1.0f, centerX, centerY });
        } else {
            const float scale = distance / pinchStartDistance;
            if (std::fabs(scale - pinchScale) > 0.005f || centerX != pinchX || centerY != pinchY) {
                pinchScale = scale;
                emit(out, count, { GestureType::PinchMove, centerX, centerY, 0, 0, scale, pinchStartX, pinchStartY });
            }
        }
        pinchX = centerX;
        pinchY = centerY;
        fingers = 2;
        return count;
    }

    if (pinching) {
        // Lifting one finger ends the pinch where it was
        pinching = false;
        emit(out, count, { GestureType::PinchEnd, pinchX, pinchY, 0, 0, pinchScale, pinchStartX, pinchStartY });
    }

    if (sample.count == 0) {
        if (fingers == 1 && !spent && !moved && !longPressed) {
            emit(out, count, { GestureType::Tap, lastX, lastY, 0, 0, 1.0f, lastX, lastY });
            if (pendingTap && now - pendingAt < DOUBLE_TAP_MS && std::abs(pendingX - lastX) < DOUBLE_TAP_DISTANCE &&
                std::abs(pendingY - lastY) < DOUBLE_TAP_DISTANCE) {
                pendingTap = false;
                emit(out, count, { GestureType::DoubleTap, lastX, lastY, 0, 0, 1.0f, pendingX, pendingY });
            } else {
                pendingTap = true;
                pendingAt = now;
                pendingX = lastX;
                pendingY = lastY;
            }
        }
        fingers = 0;
        spent = false;
        return count;
    }

    const int x = sample.x[0];
    const int y = sample.y[0];
    if (fingers == 0) {
        fingers = 1;
        startX = x;
        startY = y;
        lastX = x;
        lastY = y;
        downAt = now;
        moved = false;
        longPressed = false;
        return count;
    }
    fingers = 1;
    if (spent) {
        lastX = x;
        lastY = y;
        return count;
    }
    if (!moved && (std::abs(x - startX) > TAP_SLOP || std::abs(y - startY) > TAP_SLOP)) {
        moved = true;
        // The movement inside the slop counts too, so the map does not lag behind the finger
        lastX = startX;
        lastY = startY;
    }
    if (moved && (x != lastX || y != lastY)) {
        emit(out, count, { GestureType::Drag, x, y, x - lastX, y - lastY, 1.0f, startX, startY });
    }
    if (!moved && !longPressed && now - downAt >= LONG_PRESS_MS) {
        longPressed = true;
        pendingTap = false;
        emit(out, count, { GestureType::LongPress, x, y, 0, 0, 1.0f, startX, startY });
    }
    lastX = x;
    lastY = y;
    return count;
}

} // namespace maps
