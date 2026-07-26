#pragma once

#include <tactility/device.h>
#include <tactility/drivers/pointer.h>

/**
 * Wrapper for pointer_* device driver functions
 */
class TouchDriver {

    struct Device* device;

public:

    explicit TouchDriver(struct Device* device) : device(device) {
        device_get(device);
    }

    ~TouchDriver() {
        device_put(device);
    }

    bool getTouchedPoints(uint16_t* x, uint16_t* y, uint16_t* strength, uint8_t* count, uint8_t maxCount) const {
        // Poll without blocking: perform one read attempt, then report whatever is cached.
        pointer_read_data(device, 0);
        return pointer_get_touched_points(device, x, y, strength, count, maxCount);
    }
};
