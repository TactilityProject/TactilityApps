// SPDX-License-Identifier: Apache-2.0
#include "Panel.h"

#include "Ili9341Spi.h"

#include <driver/spi_master.h>
#include <esp_err.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/log.h>

#include <new>

namespace ili9341panel {

namespace {

constexpr auto* TAG = "Ili9341Panel";
constexpr auto* DEVICE_NAME = "ili9341-panel";

struct PanelRuntime {
    struct Device device {};
    struct Ili9341SpiConfig driver_config {};
    bool bus_initialized = false;
    bool device_added = false;
};

PanelRuntime runtime;

} // namespace

error_t startPanel(const struct Ili9341PanelConfig& config) {
    if (!config.enabled || runtime.device_added) {
        return ERROR_NONE;
    }

    spi_bus_config_t bus = {};
    bus.mosi_io_num = config.pin_mosi;
    bus.miso_io_num = config.pin_miso;
    bus.sclk_io_num = config.pin_sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.data4_io_num = -1;
    bus.data5_io_num = -1;
    bus.data6_io_num = -1;
    bus.data7_io_num = -1;
    bus.data_io_default_level = false;
    bus.max_transfer_sz = static_cast<int>(config.max_transfer_bytes);
    bus.dma_burst_size = 0;
    bus.flags = 0;
    bus.isr_cpu_id = ESP_INTR_CPU_AFFINITY_AUTO;
    bus.intr_flags = 0;

    const auto host = static_cast<spi_host_device_t>(config.spi_host);
    esp_err_t bus_result = spi_bus_initialize(host, &bus, SPI_DMA_CH_AUTO);
    if (bus_result != ESP_OK) {
        LOG_E(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(bus_result));
        return ERROR_RESOURCE;
    }
    runtime.bus_initialized = true;

    runtime.driver_config = Ili9341SpiConfig {
        .spi_host = config.spi_host,
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
    runtime.device.parent = nullptr;
    runtime.device.flags = 0;

    if (device_construct_add_start(&runtime.device, ILI9341_SPI_COMPATIBLE) != ERROR_NONE) {
        LOG_E(TAG, "Failed to start the ILI9341 panel");
        spi_bus_free(host);
        runtime.bus_initialized = false;
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

    if (result == ERROR_NONE && runtime.bus_initialized) {
        const auto host = static_cast<spi_host_device_t>(runtime.driver_config.spi_host);
        if (spi_bus_free(host) != ESP_OK) {
            LOG_W(TAG, "Could not free the SPI bus");
            result = ERROR_RESOURCE;
        } else {
            runtime.bus_initialized = false;
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
