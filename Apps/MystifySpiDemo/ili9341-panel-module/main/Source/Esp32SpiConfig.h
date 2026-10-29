// SPDX-License-Identifier: Apache-2.0
#pragma once

/**
 * Mirror of `platform-esp32`'s `tactility/drivers/esp32_spi.h`.
 *
 * The platform's header is not shipped in the app SDK, but an app-packaged module can still create
 * an esp32 SPI controller device by passing a config with this exact layout to
 * device_construct_add_start(..., "espressif,esp32-spi"). Keep it in sync with the platform header.
 */

#include <driver/spi_master.h>
#include <tactility/drivers/gpio.h>

#include <cstdint>

struct Esp32SpiConfig {
    spi_host_device_t host;
    /** Clock pin */
    struct GpioPinSpec pin_sclk;
    /** Data 0 pin */
    struct GpioPinSpec pin_mosi;
    /** Data 1 pin */
    struct GpioPinSpec pin_miso;
    /** Data 2 pin */
    struct GpioPinSpec pin_wp;
    /** Data 3 pin */
    struct GpioPinSpec pin_hd;
    /** Data transfer size limit in bytes. 0 means the platform decides the limit. */
    int max_transfer_size;
    /** Array of chip select GPIO pin specs */
    struct GpioPinSpec* cs_gpios;
    /** The item count of cs_gpios */
    uint8_t cs_gpios_count;
    /** Weak internal pull-up on MISO. */
    bool miso_pull_up;
};
