// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <tactility/device.h>
#include <tactility/driver.h>

#include <cstdint>

/**
 * ILI9341 SPI display driver.
 *
 * Used by the `ili9341-panel-module` kernel module in `Apps/MystifySpiDemo`, which owns the SPI
 * bus, creates the display device from persisted settings, and tears it down again. The module is
 * loaded from the app package at boot, so the panel exists as a normal DisplayApi device even when
 * no app is running.
 *
 * The driver uses ESP-IDF's spi_master polling transactions directly instead of esp_lcd. That
 * makes draw_bitmap() fully synchronous with no completion callback: an app/module ELF is not
 * guaranteed to be executable while the firmware's SPI ISR runs with caches disabled, and a
 * callback into it could crash there.
 */

/** What the panel looks like after MADCTL has been applied (its logical resolution). */
struct Ili9341SpiConfig {
    /** ESP-IDF SPI host that is already initialised with the panel's bus pins (e.g. SPI2_HOST). */
    int spi_host = 1;

    uint16_t width = 320;
    uint16_t height = 240;

    /** MADCTL row/column exchange. Together with width/height this selects the landscape mode. */
    bool swap_xy = true;
    bool mirror_x = false;
    bool mirror_y = false;
    /** Send INVON. If the image looks like a photographic negative, this is the knob. */
    bool invert_color = true;
    /** BGR element order, as used by M5Stack/LCDWiki modules. */
    bool bgr_order = true;

    uint32_t pixel_clock_hz = 40 * 1000 * 1000;

    /** Largest single SPI transaction. Must not exceed the bus' max_transfer_sz. */
    uint16_t max_transfer_bytes = 16 * 1024;

    /** Panel control pins. Set to -1 when not wired. */
    int pin_dc = -1;
    int pin_reset = -1;
    int pin_cs = -1;
    bool reset_active_high = false;

    /** Read and log the panel ID once at startup (needs MISO wired). */
    bool log_panel_id = true;

    /**
     * Bring-up aid: when the panel doesn't answer, drive DC/RESET/CS one at a time so they can be
     * identified with a multimeter at the panel connector (verifies the adapter's pin mapping).
     */
    bool probe_control_pins_on_id_failure = false;
};

/** The driver instance. Register with driver_construct_add() and unregister on shutdown. */
extern "C" Driver ili9341_spi_driver;

/** Compatible string for device_construct_add_start(). */
inline const char* const ILI9341_SPI_COMPATIBLE = "tactilityapps,ili9341-spi";
