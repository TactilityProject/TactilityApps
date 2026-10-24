#pragma once

#include <tactility/device.h>
#include <tactility/drivers/mouse.h>
#include <tactility/drivers/touch.h>

/**
 * Wrapper for touch_* and mouse_* device driver functions.
 * Either device can be absent. A press of the primary mouse button counts as a touch.
 */
class TouchDriver {

    struct Device* touchDevice;
    struct Device* mouseDevice;

public:

    TouchDriver(struct Device* touchDevice, struct Device* mouseDevice) : touchDevice(touchDevice), mouseDevice(mouseDevice) {
        if (touchDevice != nullptr) {
            device_get(touchDevice);
        }
        if (mouseDevice != nullptr) {
            device_get(mouseDevice);
        }
    }

    ~TouchDriver() {
        if (touchDevice != nullptr) {
            device_put(touchDevice);
        }
        if (mouseDevice != nullptr) {
            device_put(mouseDevice);
        }
    }

    bool isTouched() const {
        if (touchDevice != nullptr) {
            uint16_t x, y, strength;
            uint8_t count = 0;
            // Poll without blocking: perform one read attempt, then report whatever is cached.
            touch_read_data(touchDevice, 0);
            if (touch_get_touched_points(touchDevice, &x, &y, &strength, &count, 1)) {
                return true;
            }
        }
        bool pressed = false;
        return mouseDevice != nullptr && mouse_get_button_pressed(mouseDevice, 0, &pressed) == ERROR_NONE && pressed;
    }
};
