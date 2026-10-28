// SPDX-License-Identifier: Apache-2.0
#pragma once

/**
 * Shared LVGL binding helpers for the ILI9341 panel. Used by the settings app, the dashboard and
 * the CLI so they all bind the device exactly like the lvgl module's boot-time scan does.
 */

#include <lvgl/devices/device_context.h>
#include <lvgl/devices/display.h>
#include <lvgl/lvgl.h>

#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/error.h>

namespace ili9341panelui {

/** @return the lv_display bound to @a device, or nullptr. */
inline lv_display_t* findLvglDisplay(struct Device* device) {
    for (lv_display_t* display = lv_display_get_next(nullptr); display != nullptr; display = lv_display_get_next(display)) {
        auto* context = static_cast<LvglDeviceContext*>(lv_display_get_driver_data(display));
        if (context != nullptr && context->device == device) {
            return display;
        }
    }
    return nullptr;
}

/** Removes every lv_display bound to @a device. Caller must hold the LVGL lock. */
inline void removeLvglDisplays(struct Device* device) {
    lv_display_t* display = lv_display_get_next(nullptr);
    while (display != nullptr) {
        lv_display_t* next = lv_display_get_next(display);
        auto* context = static_cast<LvglDeviceContext*>(lv_display_get_driver_data(display));
        if (context != nullptr && context->device == device) {
            lvgl_display_remove(display);
        }
        display = next;
    }
}

/**
 * Detaches the panel from LVGL so its device can be stopped or drawn to directly.
 * @param[in] clear fills the panel before detaching, so it doesn't keep a stale frame
 */
inline void detachFromLvgl(struct Device* device, bool clear = false) {
    if (clear) {
        display_clear(device);
    }
    lvgl_lock();
    removeLvglDisplays(device);
    lvgl_unlock();
}

/** Binds @a device as an lv_display with the same configuration the lvgl module uses at boot. */
inline error_t attachToLvgl(struct Device* device) {
    const uint16_t vertical_resolution = display_get_resolution_y(device);
    const enum DisplayColorFormat format = display_get_color_format(device);
    const bool can_hw_rotate =
        display_has_capability(device, DISPLAY_CAPABILITY_CAP_SWAP_XY) &&
        display_has_capability(device, DISPLAY_CAPABILITY_CAP_MIRROR);

    const LvglDisplayConfig config {
        .buffer_height = static_cast<uint16_t>(vertical_resolution > 10 ? vertical_resolution / 10 : vertical_resolution),
        .double_buffer = false,
        .sw_rotate = !can_hw_rotate,
        .swap_bytes = format == DISPLAY_COLOR_FORMAT_RGB565_SWAPPED ||
            format == DISPLAY_COLOR_FORMAT_BGR565_SWAPPED ||
            format == DISPLAY_COLOR_FORMAT_BGR565,
        .force_full_frame = display_has_capability(device, DISPLAY_CAPABILITY_REQUIRES_FULL_FRAME),
        .prefer_external_ram = display_has_capability(device, DISPLAY_CAPABILITY_PREFER_EXTERNAL_RAM),
    };

    lvgl_lock();
    // A device that was stopped and re-created can leave an older display pointing at it.
    removeLvglDisplays(device);
    lv_display_t* display = nullptr;
    const error_t result = lvgl_display_add(device, &config, &display);
    lvgl_unlock();
    return result;
}

} // namespace ili9341panelui
