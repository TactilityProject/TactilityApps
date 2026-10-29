// SPDX-License-Identifier: Apache-2.0
#include "Panel.h"

#include "Esp32SpiConfig.h"
#include "Ili9341Spi.h"

#include <driver/spi_master.h>
#include <esp_err.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/gpio.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <new>

namespace ili9341panel {

namespace {

constexpr auto* TAG = "Ili9341Panel";
constexpr auto* DEVICE_NAME = "ili9341-panel";
constexpr auto* SPI_CONTROLLER_NAME = "ili9341-spi";
constexpr auto* SPI_CONTROLLER_COMPATIBLE = "espressif,esp32-spi";

struct PanelRuntime {
    // The SPI controller is created from the panel settings, so the module doesn't depend on a
    // board devicetree node for the M-BUS pins.
    struct Device controller {};
    struct Esp32SpiConfig controller_config {};
    bool controller_added = false;

    struct Device device {};
    struct Ili9341SpiConfig driver_config {};
    bool device_added = false;
};

PanelRuntime runtime;

struct GpioPinSpec makePin(struct Device* gpio_controller, int32_t pin) {
    if (gpio_controller == nullptr || pin < 0) {
        return GPIO_PIN_SPEC_NONE;
    }
    return GpioPinSpec {
        gpio_controller,
        static_cast<gpio_pin_t>(pin),
        GPIO_FLAG_NONE,
    };
}

} // namespace

error_t startPanel(const struct Ili9341PanelConfig& config) {
    if (!config.enabled || runtime.device_added) {
        return ERROR_NONE;
    }

    struct Device* gpio0 = nullptr;
    if (device_get_by_name("gpio0", &gpio0) != ERROR_NONE || gpio0 == nullptr) {
        LOG_E(TAG, "The native GPIO controller (gpio0) was not found");
        return ERROR_NOT_FOUND;
    }

    runtime.controller_config = Esp32SpiConfig {
        .host = static_cast<spi_host_device_t>(config.spi_host),
        .pin_sclk = makePin(gpio0, config.pin_sck),
        .pin_mosi = makePin(gpio0, config.pin_mosi),
        .pin_miso = makePin(gpio0, config.pin_miso),
        .pin_wp = GPIO_PIN_SPEC_NONE,
        .pin_hd = GPIO_PIN_SPEC_NONE,
        .max_transfer_size = static_cast<int>(config.max_transfer_bytes),
        // CS is driven by the panel driver, so the controller owns no chip select pins.
        .cs_gpios = nullptr,
        .cs_gpios_count = 0,
        .miso_pull_up = false,
    };

    runtime.controller = {};
    runtime.controller.name = SPI_CONTROLLER_NAME;
    runtime.controller.address = 0;
    runtime.controller.config = &runtime.controller_config;
    runtime.controller.parent = nullptr;
    runtime.controller.flags = 0;

    if (device_construct_add_start(&runtime.controller, SPI_CONTROLLER_COMPATIBLE) != ERROR_NONE) {
        LOG_E(TAG, "Failed to start the SPI controller");
        return ERROR_RESOURCE;
    }
    runtime.controller_added = true;

    runtime.driver_config = Ili9341SpiConfig {
        .width = config.width,
        .height = config.height,
        .swap_xy = config.swap_xy,
        .mirror_x = config.mirror_x,
        .mirror_y = config.mirror_y,
        .invert_color = config.invert_color,
        .bgr_order = config.bgr_order,
        .pixel_clock_hz = config.pixel_clock_hz,
        .max_transfer_bytes = static_cast<uint16_t>(config.max_transfer_bytes),
        .pin_dc = config.pin_dc,
        .pin_reset = config.pin_reset,
        .pin_cs = config.pin_cs,
        .reset_active_high = false,
        .log_panel_id = true,
        .probe_control_pins_on_id_failure = false,
    };

    runtime.device = {};
    runtime.device.name = DEVICE_NAME;
    runtime.device.address = 0;
    runtime.device.config = &runtime.driver_config;
    runtime.device.parent = &runtime.controller;
    runtime.device.flags = 0;

    if (device_construct_add_start(&runtime.device, ILI9341_SPI_COMPATIBLE) != ERROR_NONE) {
        LOG_E(TAG, "Failed to start the ILI9341 panel");
        device_stop(&runtime.controller);
        device_remove(&runtime.controller);
        device_destruct(&runtime.controller);
        runtime.controller_added = false;
        return ERROR_RESOURCE;
    }
    runtime.device_added = true;

    LOG_I(
        TAG,
        "Panel ready: %ux%u, SPI %u Hz",
        static_cast<unsigned>(config.width),
        static_cast<unsigned>(config.height),
        static_cast<unsigned>(config.pixel_clock_hz)
    );
    return ERROR_NONE;
}

error_t stopPanel() {
    error_t result = ERROR_NONE;

    if (runtime.device_added) {
        error_t stop_result = device_stop(&runtime.device);
        error_t remove_result = stop_result == ERROR_NONE ? device_remove(&runtime.device) : ERROR_INVALID_STATE;
        error_t destruct_result = remove_result == ERROR_NONE ? device_destruct(&runtime.device) : ERROR_INVALID_STATE;
        if (destruct_result != ERROR_NONE) {
            LOG_W(TAG, "Could not tear down the display device (still in use)");
            result = ERROR_RESOURCE_BUSY;
        } else {
            runtime.device_added = false;
        }
    }

    // The controller can only stop once its peripherals (the panel) are gone.
    if (result == ERROR_NONE && runtime.controller_added) {
        error_t stop_result = device_stop(&runtime.controller);
        error_t remove_result = stop_result == ERROR_NONE ? device_remove(&runtime.controller) : ERROR_INVALID_STATE;
        error_t destruct_result = remove_result == ERROR_NONE ? device_destruct(&runtime.controller) : ERROR_INVALID_STATE;
        if (destruct_result != ERROR_NONE) {
            LOG_W(TAG, "Could not tear down the SPI controller");
            result = ERROR_RESOURCE;
        } else {
            runtime.controller_added = false;
        }
    }

    return result;
}

bool isPanelActive() {
    return runtime.device_added && device_is_ready(&runtime.device);
}

struct Device* getPanelDevice() {
    if (!runtime.device_added) {
        return nullptr;
    }
    if (device_get(&runtime.device) != ERROR_NONE) {
        return nullptr;
    }
    return &runtime.device;
}

} // namespace ili9341panel
