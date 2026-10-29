// SPDX-License-Identifier: GPL-3.0-or-later
#include "Input.h"

#include "../platform/Platform.h"

#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/mouse.h>
#include <tactility/drivers/touch.h>

namespace maps {

namespace {

constexpr uint32_t RESCAN_INTERVAL_MS = 2000;

struct Collect {
    Device** devices;
    int* count;
};

bool collectDevice(Device* device, void* context) {
    auto* collect = static_cast<Collect*>(context);
    if (*collect->count >= Input::MAX_DEVICES || !device_is_ready(device)) {
        return true;
    }
    for (int i = 0; i < *collect->count; i++) {
        if (collect->devices[i] == device) {
            return true;
        }
    }
    if (device_get(device) == ERROR_NONE) {
        collect->devices[(*collect->count)++] = device;
    }
    return true;
}

} // namespace

Input::~Input() {
    end();
}

void Input::begin() {
    rescan();
    if (device_get_first_active_by_type(&TOUCH_TYPE, &touch) != ERROR_NONE) {
        touch = nullptr;
    }
    logInfo("Input: %d keyboards, %d mice, %s", keyboardCount, mouseCount, touch != nullptr ? "touch" : "no touch");
}

void Input::end() {
    for (int i = 0; i < keyboardCount; i++) {
        device_put(keyboards[i]);
    }
    keyboardCount = 0;
    for (int i = 0; i < mouseCount; i++) {
        device_put(mice[i]);
    }
    mouseCount = 0;
    if (touch != nullptr) {
        device_put(touch);
        touch = nullptr;
    }
}

void Input::rescan() {
    lastScanMs = nowMs();
    Collect keyboardCollect { keyboards, &keyboardCount };
    device_for_each_of_type(&KEYBOARD_TYPE, &keyboardCollect, collectDevice);
    Collect mouseCollect { mice, &mouseCount };
    device_for_each_of_type(&MOUSE_TYPE, &mouseCollect, collectDevice);
}

int Input::readKeys(KeyEvent* events, int maxEvents) {
    // The trackball driver swaps between a keyboard and a mouse device when its settings change
    if (nowMs() - lastScanMs > RESCAN_INTERVAL_MS) {
        rescan();
    }
    int count = 0;
    for (int i = 0; i < keyboardCount && count < maxEvents; i++) {
        if (!device_is_ready(keyboards[i])) {
            continue;
        }
        KeyboardKeyData data;
        while (count < maxEvents && keyboard_read_key(keyboards[i], &data) == ERROR_NONE && data.key != 0) {
            events[count++] = { data.key, data.pressed };
            if (!data.continue_reading) {
                break;
            }
        }
    }
    return count;
}

bool Input::readMouse(int32_t& dx, int32_t& dy) {
    dx = 0;
    dy = 0;
    bool pressed = false;
    for (int i = 0; i < mouseCount; i++) {
        if (!device_is_ready(mice[i])) {
            continue;
        }
        int32_t x = 0;
        int32_t y = 0;
        if (mouse_read_delta(mice[i], &x, &y) == ERROR_NONE) {
            dx += x;
            dy += y;
        }
        bool button = false;
        if (mouse_get_button_count(mice[i]) > 0 && mouse_get_button_pressed(mice[i], 0, &button) == ERROR_NONE && button) {
            pressed = true;
        }
    }
    return pressed;
}

bool Input::readTouch(TouchState& state) {
    state.count = 0;
    if (touch == nullptr || touch_read_data(touch, 0) != ERROR_NONE) {
        return false;
    }
    uint16_t strength[2];
    uint8_t count = 0;
    if (!touch_get_touched_points(touch, state.x, state.y, strength, &count, 2)) {
        return false;
    }
    state.count = count > 2 ? 2 : count;
    return state.count > 0;
}

} // namespace maps
