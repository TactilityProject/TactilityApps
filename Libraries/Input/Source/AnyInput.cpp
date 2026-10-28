// SPDX-License-Identifier: Apache-2.0
#include <input/AnyInput.h>

#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/mouse.h>
#include <tactility/drivers/touch.h>
#include <tactility/drivers/trackball.h>
#include <tactility/error.h>

#include <Tactility/kernel/Kernel.h>

namespace {

constexpr uint32_t POLL_INTERVAL_MS = 20;

struct KeyboardCollection {
    struct Device* devices[4] {};
    int count = 0;
};

bool collectKeyboard(struct Device* device, void* context) {
    auto* collection = static_cast<KeyboardCollection*>(context);
    if (collection->count >= 4) {
        return false;
    }
    if (device_is_ready(device) && device_get(device) == ERROR_NONE) {
        collection->devices[collection->count++] = device;
    }
    return true;
}

} // namespace

AnyInput::AnyInput() {
    KeyboardCollection collection {};
    device_for_each_of_type(&KEYBOARD_TYPE, &collection, collectKeyboard);
    keyboard_count_ = collection.count;
    for (int i = 0; i < keyboard_count_; i++) {
        keyboards_[i] = collection.devices[i];
    }

    if (device_get_first_active_by_type(&TOUCH_TYPE, &touch_) != ERROR_NONE) {
        touch_ = nullptr;
    }
    if (device_get_first_active_by_type(&TRACKBALL_TYPE, &trackball_) != ERROR_NONE) {
        trackball_ = nullptr;
    }
    if (device_get_first_active_by_type(&MOUSE_TYPE, &mouse_) != ERROR_NONE) {
        mouse_ = nullptr;
    }
}

AnyInput::~AnyInput() {
    for (int i = 0; i < keyboard_count_; i++) {
        device_put(keyboards_[i]);
    }
    if (touch_ != nullptr) {
        device_put(touch_);
    }
    if (trackball_ != nullptr) {
        device_put(trackball_);
    }
    if (mouse_ != nullptr) {
        device_put(mouse_);
    }
}

bool AnyInput::available() const {
    return touch_ != nullptr || trackball_ != nullptr || mouse_ != nullptr || keyboard_count_ > 0;
}

bool AnyInput::activity() {
    bool active = false;

    if (touch_ != nullptr) {
        const bool touched = touch_read_data(touch_, 0) == ERROR_NONE
            && [this]() {
                uint16_t x, y, strength;
                uint8_t count = 0;
                return touch_get_touched_points(touch_, &x, &y, &strength, &count, 1) && count > 0;
            }();
        if (touched && !touch_down_) {
            active = true;
        }
        touch_down_ = touched;
    }

    for (int i = 0; i < keyboard_count_; i++) {
        struct KeyboardKeyData data {};
        while (keyboard_read_key(keyboards_[i], &data) == ERROR_NONE && data.key != 0) {
            if (data.pressed) {
                active = true;
            }
            if (!data.continue_reading) {
                break;
            }
        }
    }

    if (trackball_ != nullptr) {
        bool pressed = false;
        if (trackball_get_button_pressed(trackball_, &pressed) == ERROR_NONE) {
            if (pressed && !trackball_down_) {
                active = true;
            }
            trackball_down_ = pressed;
        }
    }

    if (mouse_ != nullptr) {
        bool pressed = false;
        if (mouse_get_button_pressed(mouse_, 0, &pressed) == ERROR_NONE) {
            if (pressed && !mouse_down_) {
                active = true;
            }
            mouse_down_ = pressed;
        }
    }

    return active;
}

bool AnyInput::wait(uint32_t timeout_ms) {
    uint32_t waited_ms = 0;

    while (true) {
        if (activity()) {
            return true;
        }
        if (timeout_ms != UINT32_MAX && waited_ms >= timeout_ms) {
            return false;
        }

        tt::kernel::delayTicks(tt::kernel::millisToTicks(POLL_INTERVAL_MS));
        if (timeout_ms != UINT32_MAX) {
            waited_ms += POLL_INTERVAL_MS;
        }
    }
}
