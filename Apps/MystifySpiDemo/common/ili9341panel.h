// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tactility/device.h>
#include <tactility/error.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Configuration for the app-owned ILI9341 SPI panel.
 *
 * The defaults match the M5Stack Tab5 M-BUS wiring used by this package:
 * SCK=5, MOSI=18, MISO=19, DC=47, RESET=4, CS=51, landscape 320x240.
 */
struct Ili9341PanelConfig {
    /** Create the device on module start. The settings are still saved while false. */
    bool enabled;

    /** ESP-IDF SPI host (SPI2_HOST is 1). */
    int32_t spi_host;
    int32_t pin_sck;
    int32_t pin_mosi;
    int32_t pin_miso;
    int32_t pin_dc;
    int32_t pin_reset;
    int32_t pin_cs;

    /** Logical resolution after MADCTL is applied. */
    uint16_t width;
    uint16_t height;
    bool swap_xy;
    bool mirror_x;
    bool mirror_y;
    bool invert_color;
    bool bgr_order;

    uint32_t pixel_clock_hz;
    uint32_t max_transfer_bytes;
};

/** Fills @a out_config with the built-in defaults. */
error_t ili9341panel_settings_defaults(struct Ili9341PanelConfig* out_config);

/** Loads the saved settings, or the defaults when nothing was saved yet. */
error_t ili9341panel_settings_load(struct Ili9341PanelConfig* out_config);

/** Persists the settings. They take effect on the next ili9341panel_apply() or module start. */
error_t ili9341panel_settings_save(const struct Ili9341PanelConfig* config);

/**
 * Stops the panel (if active) and re-creates it from the saved settings.
 *
 * LVGL must not be bound to the display while this runs: remove the panel's lv_display_t first
 * (the settings app does), or stop LVGL entirely (as the demo app does).
 */
error_t ili9341panel_apply(void);

/** @return true while the display device exists and is started. */
bool ili9341panel_is_active(void);

/**
 * Returns a new reference to the active display device.
 * The caller must call device_put() when done.
 * @retval ERROR_NOT_FOUND the panel is not active
 */
error_t ili9341panel_get_device(struct Device** out_device);

#ifdef __cplusplus
}
#endif
