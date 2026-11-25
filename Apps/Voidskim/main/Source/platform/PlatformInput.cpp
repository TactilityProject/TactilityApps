#include "PlatformInput.h"

#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/touch.h>
#include <tactility/drivers/trackball.h>
#include <tactility/log.h>

using namespace hd;

constexpr auto* TAG = "VoidskimInput";
// After a press, an action stays held at least this long (for keyboards without real releases)
constexpr uint32_t MIN_HOLD_MS = 130;
// How long one trackball roll step holds a direction
constexpr uint32_t TRACKBALL_HOLD_MS = 160;

namespace {

int actionIndex(uint32_t action) {
    for (int i = 0; i < 32; i++) {
        if (action == (1u << i)) return i;
    }
    return -1;
}

bool isTrackballChild(Device* device) {
    Device* parent = device_get_parent(device);
    return parent != nullptr && device_get_type(parent) == &TRACKBALL_TYPE;
}

} // namespace

void PlatformInput::init(const DisplayOutput* displayOutput) {
    output = displayOutput;
    keyboardCount = 0;
    trackballCount = 0;
    device_for_each_of_type(&KEYBOARD_TYPE, this, [](Device* device, void* context) -> bool {
        auto* self = static_cast<PlatformInput*>(context);
        // A trackball in keyboard mode is read directly instead
        if (!isTrackballChild(device) && self->keyboardCount < MAX_KEYBOARDS) {
            device_get(device);
            self->keyboards[self->keyboardCount++] = device;
        }
        return self->keyboardCount < MAX_KEYBOARDS;
    });
    device_for_each_of_type(&TRACKBALL_TYPE, this, [](Device* device, void* context) -> bool {
        auto* self = static_cast<PlatformInput*>(context);
        if (self->trackballCount < MAX_TRACKBALLS) {
            device_get(device);
            self->trackballs[self->trackballCount++] = device;
        }
        return self->trackballCount < MAX_TRACKBALLS;
    });
    if (device_get_first_active_by_type(&TOUCH_TYPE, &touch) != ERROR_NONE) {
        touch = nullptr;
    }
    LOG_I(TAG, "%d keyboard(s), %d trackball(s), %s", keyboardCount, trackballCount, touch != nullptr ? "touch" : "no touch");
}

void PlatformInput::shutdown() {
    for (int i = 0; i < keyboardCount; i++) device_put(keyboards[i]);
    for (int i = 0; i < trackballCount; i++) device_put(trackballs[i]);
    if (touch != nullptr) device_put(touch);
    keyboardCount = 0;
    trackballCount = 0;
    touch = nullptr;
}

void PlatformInput::handleKey(uint32_t key, bool pressed, uint32_t nowMs, InputState& state) {
    key = normaliseKey(key);
    if (key == 0) {
        return;
    }
    // Some drivers report a held key as pressed again on every read: only act on changes
    int index = -1;
    for (int i = 0; i < heldKeyCount; i++) {
        if (heldKeys[i] == key) index = i;
    }
    if (pressed) {
        if (index >= 0) return;
        // The actions are fixed at the press, so rebinding a held key can't leave it stuck
        const uint32_t actions = bindings != nullptr ? bindings->actionsFor(key) : 0;
        if (heldKeyCount < MAX_HELD_KEYS) {
            heldKeys[heldKeyCount] = key;
            heldActions[heldKeyCount] = actions;
            heldKeyCount++;
        }
        for (int i = 0; i < ACTION_COUNT; i++) {
            if (actions & (1u << i)) lastPress[i] = nowMs;
        }
        state.keyPressed = true;
        if (state.keysPressedCount < InputState::MAX_KEYS_PRESSED) {
            state.keysPressed[state.keysPressedCount++] = key;
        }
    } else {
        if (index < 0) return;
        heldKeyCount--;
        heldKeys[index] = heldKeys[heldKeyCount];
        heldActions[index] = heldActions[heldKeyCount];
    }
}

void PlatformInput::poll(InputState& state, uint32_t nowMs) {
    state.keyPressed = false;
    state.keysPressedCount = 0;
    for (int k = 0; k < keyboardCount; k++) {
        if (!device_is_ready(keyboards[k])) continue;
        KeyboardKeyData data = {};
        int guard = 0;
        while (keyboard_read_key(keyboards[k], &data) == ERROR_NONE && guard++ < 16) {
            handleKey(data.key, data.pressed, nowMs, state);
            if (!data.continue_reading) break;
            data = {};
        }
    }

    // Trackball: each roll step holds its direction briefly, so continuous rolling steers smoothly
    bool trackballClick = false;
    for (int t = 0; t < trackballCount; t++) {
        if (!device_is_ready(trackballs[t])) continue;
        int32_t dx = 0, dy = 0;
        if (trackball_read_delta(trackballs[t], &dx, &dy) == ERROR_NONE) {
            const uint32_t until = nowMs + TRACKBALL_HOLD_MS;
            if (dx < 0) trackballUntil[actionIndex(ActionLeft)] = trackballUntil[actionIndex(ActionSteerLeft)] = until;
            if (dx > 0) trackballUntil[actionIndex(ActionRight)] = trackballUntil[actionIndex(ActionSteerRight)] = until;
            if (dy < 0) trackballUntil[actionIndex(ActionUp)] = trackballUntil[actionIndex(ActionAccelerate)] = until;
            if (dy > 0) trackballUntil[actionIndex(ActionDown)] = trackballUntil[actionIndex(ActionBrake)] = until;
        }
        bool pressed = false;
        if (trackball_get_button_pressed(trackballs[t], &pressed) == ERROR_NONE && pressed) {
            trackballClick = true;
        }
    }

    uint32_t keyHeld = 0;
    for (int i = 0; i < heldKeyCount; i++) {
        keyHeld |= heldActions[i];
    }
    state.held = 0;
    for (int i = 0; i < ACTION_COUNT; i++) {
        const bool fromKey = (keyHeld & (1u << i)) || (nowMs - lastPress[i] < MIN_HOLD_MS && lastPress[i] != 0);
        const bool fromTrackball = (int32_t)(trackballUntil[i] - nowMs) > 0;
        if (fromKey || fromTrackball) state.held |= 1u << i;
    }
    if (trackballClick) state.held |= ActionBoost | ActionConfirm;

    // Touch
    state.touchAvailable = touch != nullptr;
    state.touchCount = 0;
    if (touch != nullptr && output != nullptr) {
        uint16_t xs[InputState::MAX_TOUCHES] = {};
        uint16_t ys[InputState::MAX_TOUCHES] = {};
        uint16_t strengths[InputState::MAX_TOUCHES] = {};
        uint8_t count = 0;
        touch_read_data(touch, 0);
        if (touch_get_touched_points(touch, xs, ys, strengths, &count, InputState::MAX_TOUCHES)) {
            for (int i = 0; i < count && i < InputState::MAX_TOUCHES; i++) {
                int x, y;
                if (output->mapTouch(xs[i], ys[i], x, y)) {
                    state.touches[state.touchCount++] = { x, y };
                }
            }
        }
    }
}
