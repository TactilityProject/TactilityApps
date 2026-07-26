#pragma once

#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <Tactility/kernel/Kernel.h>

/**
 * Wrapper for display_* device driver functions
 */
class DisplayDriver {

    struct Device* device;

public:

    explicit DisplayDriver(struct Device* device) : device(device) {
        device_get(device);
    }

    ~DisplayDriver() {
        device_put(device);
    }

    bool lock(TickType_t timeout = tt::kernel::MAX_TICKS) const {
        return device_try_lock(device, timeout);
    }

    void unlock() const {
        device_unlock(device);
    }

    uint16_t getWidth() const {
        return display_get_resolution_x(device);
    }

    uint16_t getHeight() const {
        return display_get_resolution_y(device);
    }

    enum DisplayColorFormat getColorFormat() const {
        return display_get_color_format(device);
    }

    void drawBitmap(int xStart, int yStart, int xEnd, int yEnd, const void* pixelData) const {
        display_draw_bitmap(device, xStart, yStart, xEnd, yEnd, pixelData);
    }
};
