// SPDX-License-Identifier: Apache-2.0
#include "Ili9341Spi.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <algorithm>
#include <cstring>
#include <new>

constexpr auto* TAG = "Ili9341Spi";

namespace {

// MIPI DCS commands used here (ILI9341 datasheet section 8). Kept local so the driver does not
// need ESP-IDF's esp_lcd headers, which are a firmware-internal dependency.
constexpr uint8_t CMD_SWRESET = 0x01;
constexpr uint8_t CMD_RDDID = 0x04;
constexpr uint8_t CMD_RDD_MADCTL = 0x0B;
constexpr uint8_t CMD_SLPIN = 0x10;
constexpr uint8_t CMD_SLPOUT = 0x11;
constexpr uint8_t CMD_INVOFF = 0x20;
constexpr uint8_t CMD_INVON = 0x21;
constexpr uint8_t CMD_DISPOFF = 0x28;
constexpr uint8_t CMD_DISPON = 0x29;
constexpr uint8_t CMD_CASET = 0x2A;
constexpr uint8_t CMD_RASET = 0x2B;
constexpr uint8_t CMD_RAMWR = 0x2C;
constexpr uint8_t CMD_MADCTL = 0x36;
constexpr uint8_t CMD_COLMOD = 0x3A;

// MADCTL bits
constexpr uint8_t MADCTL_MY = 1 << 7;
constexpr uint8_t MADCTL_MX = 1 << 6;
constexpr uint8_t MADCTL_MV = 1 << 5;
constexpr uint8_t MADCTL_BGR = 1 << 3;

/** One entry of the vendor init table: command, parameters, and a post-command delay. */
struct InitCommand {
    uint8_t command;
    uint8_t data[15];
    uint8_t data_size;
    uint16_t delay_ms;
};

// Init sequence matches esp_lcd_ili9341's default (proven on real ILI9341 panels), minus the
// duplicate pump-ratio entry it carries.
constexpr InitCommand INIT_COMMANDS[] = {
    { 0xCF, { 0x00, 0xAA, 0xE0 }, 3, 0 },                                  // Power control B
    { 0xED, { 0x67, 0x03, 0x12, 0x81 }, 4, 0 },                            // Power on sequence
    { 0xE8, { 0x8A, 0x01, 0x78 }, 3, 0 },                                  // Driver timing A
    { 0xCB, { 0x39, 0x2C, 0x00, 0x34, 0x02 }, 5, 0 },                      // Power control A
    { 0xF7, { 0x20 }, 1, 0 },                                              // Pump ratio
    { 0xEA, { 0x00, 0x00 }, 2, 0 },                                        // Driver timing B
    { 0xC0, { 0x23 }, 1, 0 },                                              // Power control 1
    { 0xC1, { 0x11 }, 1, 0 },                                              // Power control 2
    { 0xC5, { 0x43, 0x4C }, 2, 0 },                                        // VCOM control 1
    { 0xC7, { 0xA0 }, 1, 0 },                                              // VCOM control 2
    { 0xB1, { 0x00, 0x1B }, 2, 0 },                                        // Frame rate
    { 0xF2, { 0x00 }, 1, 0 },                                              // 3-gamma off
    { 0x26, { 0x01 }, 1, 0 },                                              // Gamma curve 1
    { 0xE0, { 0x1F, 0x36, 0x36, 0x3A, 0x0C, 0x05, 0x4F, 0x87, 0x3C,
              0x08, 0x11, 0x35, 0x19, 0x13, 0x00 }, 15, 0 },               // Positive gamma
    { 0xE1, { 0x00, 0x09, 0x09, 0x05, 0x13, 0x0A, 0x30, 0x78, 0x43,
              0x07, 0x0E, 0x0A, 0x26, 0x2C, 0x1F }, 15, 0 },               // Negative gamma
    { 0xB7, { 0x07 }, 1, 0 },                                              // Entry mode
    { 0xB6, { 0x08, 0x82, 0x27 }, 3, 0 },                                  // Display function
};

struct State {
    spi_device_handle_t device = nullptr;
    int pin_dc = -1;
    int pin_cs = -1;
    uint8_t madctl = 0;
    int32_t gap_x = 0;
    int32_t gap_y = 0;
    size_t max_transfer_bytes = 4096;
};

State* getState(Device* device) {
    return static_cast<State*>(device_get_driver_data(device));
}

const Ili9341SpiConfig* getConfig(Device* device) {
    return static_cast<const Ili9341SpiConfig*>(device->config);
}

/**
 * One synchronous SPI transaction. `keep_cs` leaves chip select asserted so the next transaction
 * continues the same panel command (command byte -> parameters, or RAMWR -> pixel data).
 */
error_t transmit(State* state, const void* data, size_t size, bool keep_cs, bool dc_high) {
    if (state->device == nullptr || data == nullptr || size == 0 || size > state->max_transfer_bytes) {
        return ERROR_INVALID_ARGUMENT;
    }

    gpio_set_level(static_cast<gpio_num_t>(state->pin_dc), dc_high ? 1 : 0);
    if (state->pin_cs >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(state->pin_cs), 0);
    }

    spi_transaction_t transaction = {};
    transaction.length = size * 8;
    if (size <= sizeof(transaction.tx_data)) {
        // Short transfers avoid a DMA bounce buffer entirely.
        transaction.flags |= SPI_TRANS_USE_TXDATA;
        std::memcpy(transaction.tx_data, data, size);
    } else {
        transaction.tx_buffer = data;
    }
#if SOC_PSRAM_DMA_CAPABLE
    // A Gfx frame buffer usually lives in PSRAM. Ask the SPI DMA engine to read it directly
    // instead of bouncing every chunk through a temporary internal buffer. The flag is harmless
    // for buffers that turn out to be internal; the driver checks the address itself.
    transaction.flags |= SPI_TRANS_DMA_USE_PSRAM;
#endif

    const esp_err_t result = spi_device_polling_transmit(state->device, &transaction);
    if (state->pin_cs >= 0 && !keep_cs) {
        gpio_set_level(static_cast<gpio_num_t>(state->pin_cs), 1);
    }
    return result == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
}

/** Command byte, followed by its parameters (both with CS held across the boundary). */
error_t writeCommand(State* state, uint8_t command, const uint8_t* parameters = nullptr, size_t parameter_size = 0) {
    error_t error = transmit(state, &command, 1, parameter_size > 0, false);
    if (error == ERROR_NONE && parameter_size > 0) {
        error = transmit(state, parameters, parameter_size, false, true);
    }
    return error;
}

error_t writePixels(State* state, const void* data, size_t size) {
    const uint8_t command = CMD_RAMWR;
    error_t error = transmit(state, &command, 1, true, false);
    if (error != ERROR_NONE) {
        return error;
    }

    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t offset = 0;
    while (offset < size) {
        const size_t chunk = std::min(state->max_transfer_bytes, size - offset);
        const bool more = offset + chunk < size;
        error = transmit(state, bytes + offset, chunk, more, true);
        if (error != ERROR_NONE) {
            return error;
        }
        offset += chunk;
    }
    return ERROR_NONE;
}

/** Reads as many bytes as fit in `size`, discarding any extra bytes the panel returns. */
error_t readData(State* state, uint8_t* buffer, size_t size) {
    spi_transaction_t transaction = {};
    transaction.rxlength = size * 8;
    transaction.rx_buffer = buffer;
    transaction.length = 0;

    gpio_set_level(static_cast<gpio_num_t>(state->pin_dc), 1);
    if (state->pin_cs >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(state->pin_cs), 0);
    }
    const esp_err_t result = spi_device_polling_transmit(state->device, &transaction);
    if (state->pin_cs >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(state->pin_cs), 1);
    }
    return result == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
}

error_t applyMadctl(State* state) {
    return writeCommand(state, CMD_MADCTL, &state->madctl, 1);
}

void pulseReset(const Ili9341SpiConfig* config) {
    if (config->pin_reset < 0) {
        return;
    }
    const auto pin = static_cast<gpio_num_t>(config->pin_reset);
    const int active = config->reset_active_high ? 1 : 0;
    gpio_set_level(pin, active);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(pin, !active);
    // The ILI9341 needs 120 ms after a hardware reset before it accepts SLPOUT.
    vTaskDelay(pdMS_TO_TICKS(120));
}

error_t initialisePanel(State* state, const Ili9341SpiConfig* config) {
    error_t error = ERROR_NONE;

    if (config->pin_reset >= 0) {
        gpio_config_t reset_config = {};
        reset_config.mode = GPIO_MODE_OUTPUT;
        reset_config.pin_bit_mask = 1ULL << config->pin_reset;
        if (gpio_config(&reset_config) != ESP_OK) {
            LOG_E(TAG, "Failed to configure the reset pin");
            return ERROR_RESOURCE;
        }
        pulseReset(config);
    }

    // Always send a software reset as well: it is harmless after a hardware reset and covers a
    // reset line that isn't actually connected, where the panel would otherwise stay asleep.
    error = writeCommand(state, CMD_SWRESET);
    if (error != ERROR_NONE) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(120));

    error = writeCommand(state, CMD_SLPOUT);
    if (error != ERROR_NONE) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    error = applyMadctl(state);
    if (error == ERROR_NONE) {
        const uint8_t color_mode = 0x55; // 16 bits per pixel (RGB565)
        error = writeCommand(state, CMD_COLMOD, &color_mode, 1);
    }

    for (const auto& command : INIT_COMMANDS) {
        if (error != ERROR_NONE) {
            break;
        }
        error = writeCommand(state, command.command, command.data, command.data_size);
        if (command.delay_ms != 0) {
            vTaskDelay(pdMS_TO_TICKS(command.delay_ms));
        }
    }

    if (error == ERROR_NONE) {
        error = writeCommand(state, config->invert_color ? CMD_INVON : CMD_INVOFF);
    }
    if (error == ERROR_NONE) {
        error = writeCommand(state, CMD_DISPON);
    }
    return error;
}

bool readPanelId(State* state) {
    uint8_t id[4] = {};
    const uint8_t command = CMD_RDDID;
    // Read commands need CS held from the command byte through the returned data.
    error_t error = transmit(state, &command, 1, true, false);
    if (error == ERROR_NONE) {
        error = readData(state, id, sizeof(id));
    }

    const bool id_matches = error == ERROR_NONE && id[1] == 0x93 && id[2] == 0x41;
    if (id_matches) {
        LOG_I(TAG, "Panel ID: %02X %02X %02X %02X", id[0], id[1], id[2], id[3]);
        return true;
    }
    if (error == ERROR_NONE) {
        LOG_I(TAG, "Panel ID: %02X %02X %02X %02X", id[0], id[1], id[2], id[3]);
    }

    // Many ILI9341-compatible controllers don't implement RDDID and return zeros, while still
    // answering register reads. Reading back a MADCTL we just wrote proves the panel is alive.
    const uint8_t read_madctl = CMD_RDD_MADCTL;
    uint8_t madctl[2] = {};
    if (transmit(state, &read_madctl, 1, true, false) == ERROR_NONE && readData(state, madctl, sizeof(madctl)) == ERROR_NONE) {
        LOG_I(TAG, "MADCTL readback: %02X %02X (written %02X)", madctl[0], madctl[1], state->madctl);
        if (madctl[0] == state->madctl) {
            LOG_I(TAG, "Panel responds; RDDID is not implemented by this controller");
            return true;
        }
    }

    LOG_W(TAG, "Panel doesn't respond to register reads (check SDO/MISO and the panel control lines)");
    return false;
}

/**
 * Drives DC, RESET and CS high one at a time (3 seconds each) with a matching log line, so the
 * adapter's control wires can be identified at the panel connector with a multimeter. Ends with
 * RESET and CS released (high), then the caller re-initialises the panel.
 */
void probeControlPins(const Ili9341SpiConfig* config) {
    struct Pin {
        const char* name;
        int pin;
    };
    const Pin pins[] = {
        { "DC", config->pin_dc },
        { "RESET", config->pin_reset },
        { "CS", config->pin_cs },
    };

    LOG_W(TAG, "Panel didn't answer: identifying control pins (3 s each, measure at the panel)");
    for (const auto& probe : pins) {
        if (probe.pin < 0) {
            continue;
        }
        gpio_config_t output = {};
        output.mode = GPIO_MODE_OUTPUT;
        output.pin_bit_mask = 1ULL << probe.pin;
        if (gpio_config(&output) != ESP_OK) {
            continue;
        }
        gpio_set_level(static_cast<gpio_num_t>(probe.pin), 0);
    }

    for (const auto& probe : pins) {
        if (probe.pin < 0) {
            continue;
        }
        LOG_I(TAG, "%s (GPIO%d) HIGH for 3 s", probe.name, probe.pin);
        gpio_set_level(static_cast<gpio_num_t>(probe.pin), 1);
        vTaskDelay(pdMS_TO_TICKS(3000));
        gpio_set_level(static_cast<gpio_num_t>(probe.pin), 0);
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    // Leave the control lines in their idle state.
    if (config->pin_reset >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(config->pin_reset), 1);
    }
    if (config->pin_cs >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(config->pin_cs), 1);
    }
    if (config->pin_dc >= 0) {
        gpio_set_level(static_cast<gpio_num_t>(config->pin_dc), 0);
    }
}

error_t start(Device* device) {
    const auto* config = getConfig(device);
    if (config == nullptr || config->pin_cs < 0 || config->pin_dc < 0) {
        LOG_E(TAG, "Missing CS/DC pin in the configuration");
        return ERROR_INVALID_ARGUMENT;
    }

    auto* state = new (std::nothrow) State();
    if (state == nullptr) {
        return ERROR_OUT_OF_MEMORY;
    }
    state->pin_dc = config->pin_dc;
    state->pin_cs = config->pin_cs;
    state->max_transfer_bytes = config->max_transfer_bytes;
    state->madctl = (config->swap_xy ? MADCTL_MV : 0) |
        (config->mirror_x ? MADCTL_MX : 0) |
        (config->mirror_y ? MADCTL_MY : 0) |
        (config->bgr_order ? MADCTL_BGR : 0);

    gpio_config_t dc_config = {};
    dc_config.mode = GPIO_MODE_OUTPUT;
    dc_config.pin_bit_mask = 1ULL << config->pin_dc;
    if (gpio_config(&dc_config) != ESP_OK) {
        LOG_E(TAG, "Failed to configure the DC pin");
        delete state;
        return ERROR_RESOURCE;
    }
    gpio_set_level(static_cast<gpio_num_t>(config->pin_dc), 0);

    // Chip select is driven as a plain GPIO rather than by the SPI peripheral. That keeps the
    // whole panel interface under our control (and survives the bring-up probe re-configuring the
    // pin), which also makes a stubborn panel easier to diagnose.
    gpio_config_t cs_config = {};
    cs_config.mode = GPIO_MODE_OUTPUT;
    cs_config.pin_bit_mask = 1ULL << config->pin_cs;
    if (gpio_config(&cs_config) != ESP_OK) {
        LOG_E(TAG, "Failed to configure the CS pin");
        delete state;
        return ERROR_RESOURCE;
    }
    gpio_set_level(static_cast<gpio_num_t>(config->pin_cs), 1);

    spi_device_interface_config_t device_config = {};
    device_config.mode = 0;
    device_config.clock_speed_hz = static_cast<int>(config->pixel_clock_hz);
    // -1: CS is manual (see transmit()), so the SPI peripheral must not drive it.
    device_config.spics_io_num = -1;
    device_config.queue_size = 1; // polling transactions only
    // Half duplex allows receive-only transactions (the panel ID read); full duplex rejects
    // rxlength > length, which a write-only display otherwise never needs.
    device_config.flags = SPI_DEVICE_HALFDUPLEX;
    esp_err_t result = spi_bus_add_device(
        static_cast<spi_host_device_t>(config->spi_host),
        &device_config,
        &state->device
    );
    if (result != ESP_OK) {
        LOG_E(TAG, "Failed to add the SPI device: %s", esp_err_to_name(result));
        delete state;
        return ERROR_RESOURCE;
    }

    // Keep the bus for this panel: command and pixel transactions rely on CS staying asserted
    // between them, so no other device may interleave.
    spi_device_acquire_bus(state->device, portMAX_DELAY);

    error_t error = initialisePanel(state, config);
    if (error == ERROR_NONE && config->log_panel_id) {
        if (!readPanelId(state) && config->probe_control_pins_on_id_failure) {
            probeControlPins(config);
            error = initialisePanel(state, config);
        }
    }

    if (error != ERROR_NONE) {
        LOG_E(TAG, "Panel initialisation failed");
        spi_device_release_bus(state->device);
        spi_bus_remove_device(state->device);
        delete state;
        return error;
    }

    device_set_driver_data(device, state);
    LOG_I(
        TAG,
        "Panel ready: %ux%u, SPI %u Hz",
        static_cast<unsigned>(config->width),
        static_cast<unsigned>(config->height),
        static_cast<unsigned>(config->pixel_clock_hz)
    );
    return ERROR_NONE;
}

error_t stop(Device* device) {
    auto* state = getState(device);
    if (state == nullptr) {
        return ERROR_NONE;
    }

    spi_device_release_bus(state->device);
    spi_bus_remove_device(state->device);
    delete state;
    device_set_driver_data(device, nullptr);
    return ERROR_NONE;
}

error_t reset(Device* device) {
    auto* state = getState(device);
    if (state == nullptr) {
        return ERROR_INVALID_STATE;
    }
    const auto* config = getConfig(device);

    if (config->pin_reset >= 0) {
        pulseReset(config);
        return ERROR_NONE;
    }

    error_t error = writeCommand(state, CMD_SWRESET);
    vTaskDelay(pdMS_TO_TICKS(120));
    return error;
}

error_t init(Device* device) {
    auto* state = getState(device);
    if (state == nullptr) {
        return ERROR_INVALID_STATE;
    }
    error_t error = writeCommand(state, CMD_SLPOUT);
    if (error == ERROR_NONE) {
        vTaskDelay(pdMS_TO_TICKS(100));
        error = applyMadctl(state);
    }
    return error;
}

error_t drawBitmap(
    Device* device,
    int32_t x_start,
    int32_t y_start,
    int32_t x_end,
    int32_t y_end,
    const void* color_data
) {
    auto* state = getState(device);
    if (state == nullptr || color_data == nullptr) {
        return ERROR_INVALID_STATE;
    }
    if (x_start >= x_end || y_start >= y_end) {
        return ERROR_INVALID_ARGUMENT;
    }

    x_start += state->gap_x;
    x_end += state->gap_x;
    y_start += state->gap_y;
    y_end += state->gap_y;

    const uint8_t caset[4] = {
        static_cast<uint8_t>(x_start >> 8),
        static_cast<uint8_t>(x_start),
        static_cast<uint8_t>((x_end - 1) >> 8),
        static_cast<uint8_t>(x_end - 1),
    };
    const uint8_t raset[4] = {
        static_cast<uint8_t>(y_start >> 8),
        static_cast<uint8_t>(y_start),
        static_cast<uint8_t>((y_end - 1) >> 8),
        static_cast<uint8_t>(y_end - 1),
    };

    error_t error = writeCommand(state, CMD_CASET, caset, sizeof(caset));
    if (error == ERROR_NONE) {
        error = writeCommand(state, CMD_RASET, raset, sizeof(raset));
    }
    if (error == ERROR_NONE) {
        const size_t pixel_count = static_cast<size_t>(x_end - x_start) * static_cast<size_t>(y_end - y_start);
        error = writePixels(state, color_data, pixel_count * sizeof(uint16_t));
    }
    return error;
}

error_t mirror(Device* device, bool x_axis, bool y_axis) {
    auto* state = getState(device);
    if (state == nullptr) {
        return ERROR_INVALID_STATE;
    }
    state->madctl = static_cast<uint8_t>(
        x_axis ? (state->madctl | MADCTL_MX) : (state->madctl & ~MADCTL_MX)
    );
    state->madctl = static_cast<uint8_t>(
        y_axis ? (state->madctl | MADCTL_MY) : (state->madctl & ~MADCTL_MY)
    );
    return applyMadctl(state);
}

error_t swapXy(Device* device, bool swap_axes) {
    auto* state = getState(device);
    if (state == nullptr) {
        return ERROR_INVALID_STATE;
    }
    state->madctl = static_cast<uint8_t>(
        swap_axes ? (state->madctl | MADCTL_MV) : (state->madctl & ~MADCTL_MV)
    );
    return applyMadctl(state);
}

bool getSwapXy(Device* device) {
    auto* state = getState(device);
    return state != nullptr && (state->madctl & MADCTL_MV) != 0;
}

bool getMirrorX(Device* device) {
    auto* state = getState(device);
    return state != nullptr && (state->madctl & MADCTL_MX) != 0;
}

bool getMirrorY(Device* device) {
    auto* state = getState(device);
    return state != nullptr && (state->madctl & MADCTL_MY) != 0;
}

error_t setGap(Device* device, int32_t x_gap, int32_t y_gap) {
    auto* state = getState(device);
    if (state == nullptr) {
        return ERROR_INVALID_STATE;
    }
    state->gap_x = x_gap;
    state->gap_y = y_gap;
    return ERROR_NONE;
}

error_t writeDeviceCommand(Device* device, uint8_t command) {
    auto* state = getState(device);
    if (state == nullptr) {
        return ERROR_INVALID_STATE;
    }
    return writeCommand(state, command);
}

int32_t getGapX(Device* device) {
    auto* state = getState(device);
    return state != nullptr ? state->gap_x : 0;
}

int32_t getGapY(Device* device) {
    auto* state = getState(device);
    return state != nullptr ? state->gap_y : 0;
}

error_t invertColor(Device* device, bool invert) {
    return writeDeviceCommand(device, invert ? CMD_INVON : CMD_INVOFF);
}

error_t dispOnOff(Device* device, bool on) {
    return writeDeviceCommand(device, on ? CMD_DISPON : CMD_DISPOFF);
}

error_t dispSleep(Device* device, bool sleep) {
    error_t error = writeDeviceCommand(device, sleep ? CMD_SLPIN : CMD_SLPOUT);
    if (error == ERROR_NONE) {
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    return error;
}

enum DisplayColorFormat getColorFormat(Device*) {
    // The panel expects each 16-bit pixel high-byte-first over SPI, which is the swap-invariant
    // layout of DISPLAY_COLOR_FORMAT_RGB565_SWAPPED. Gfx then writes swapped values straight to
    // memory and the bus sends them unmodified.
    return DISPLAY_COLOR_FORMAT_RGB565_SWAPPED;
}

uint16_t getResolutionX(Device* device) {
    return getConfig(device)->width;
}

uint16_t getResolutionY(Device* device) {
    return getConfig(device)->height;
}

error_t getBacklight(Device*, Device**) {
    return ERROR_NOT_SUPPORTED;
}

const DisplayApi ILI9341_SPI_DISPLAY_API = {
    .capabilities = DISPLAY_CAPABILITY_CAP_MIRROR | DISPLAY_CAPABILITY_CAP_SWAP_XY |
        DISPLAY_CAPABILITY_CAP_SET_GAP | DISPLAY_CAPABILITY_INVERT_COLOR |
        DISPLAY_CAPABILITY_ON_OFF | DISPLAY_CAPABILITY_SLEEP,
    .reset = reset,
    .init = init,
    .draw_bitmap = drawBitmap,
    .clear = nullptr,
    .refresh = nullptr,
    .mirror = mirror,
    .swap_xy = swapXy,
    .get_swap_xy = getSwapXy,
    .get_mirror_x = getMirrorX,
    .get_mirror_y = getMirrorY,
    .set_gap = setGap,
    .get_gap_x = getGapX,
    .get_gap_y = getGapY,
    .invert_color = invertColor,
    .disp_on_off = dispOnOff,
    .disp_sleep = dispSleep,
    .get_color_format = getColorFormat,
    .get_resolution_x = getResolutionX,
    .get_resolution_y = getResolutionY,
    .get_frame_buffer = nullptr,
    .get_frame_buffer_count = nullptr,
    .get_backlight = getBacklight,
    .has_capability = nullptr,
    .get_shape = nullptr,
};

} // namespace

extern "C" {

Driver ili9341_spi_driver = {
    .name = "ili9341-spi",
    .compatible = (const char*[]) { "tactilityapps,ili9341-spi", nullptr },
    .start_device = start,
    .stop_device = stop,
    .probe = nullptr,
    .api = &ILI9341_SPI_DISPLAY_API,
    .device_type = &DISPLAY_TYPE,
    .internal = nullptr,
};

} // extern "C"
