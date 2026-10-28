// SPDX-License-Identifier: Apache-2.0
#include "Settings.h"

#include <app/paths.h>
#include <tactility/log.h>
#include <tactility/preferences.h>

#include <cstring>

namespace ili9341panel {

namespace {

constexpr auto* TAG = "Ili9341PanelSettings";

// The package id: the app in this package shares the same user data directory.
constexpr auto* PACKAGE_ID = "tactility.mystifyspidemo";
constexpr auto* FILE_NAME = "ili9341panel.properties";

// SPI2_HOST on ESP32-P4.
constexpr int32_t DEFAULT_SPI_HOST = 1;

class SettingsFile {
public:
    SettingsFile() {
        char path[256];
        if (app_paths_get_user_data_path(PACKAGE_ID, FILE_NAME, path, sizeof(path)) != ERROR_NONE) {
            LOG_E(TAG, "Could not resolve the settings path");
            return;
        }
        preferences_ = preferences_open(path);
    }

    ~SettingsFile() {
        if (preferences_ != nullptr) {
            preferences_close(preferences_);
        }
    }

    SettingsFile(const SettingsFile&) = delete;
    SettingsFile& operator=(const SettingsFile&) = delete;

    bool valid() const { return preferences_ != nullptr; }

    Preferences* get() { return preferences_; }

private:
    Preferences* preferences_ = nullptr;
};

void loadBool(Preferences* preferences, const char* key, bool& value) {
    bool loaded = false;
    if (preferences_opt_bool(preferences, key, &loaded)) {
        value = loaded;
    }
}

void loadInt32(Preferences* preferences, const char* key, int32_t& value) {
    int32_t loaded = 0;
    if (preferences_opt_int32(preferences, key, &loaded)) {
        value = loaded;
    }
}

void loadUint16(Preferences* preferences, const char* key, uint16_t& value) {
    int32_t loaded = 0;
    if (preferences_opt_int32(preferences, key, &loaded)) {
        value = static_cast<uint16_t>(loaded);
    }
}

void loadUint32(Preferences* preferences, const char* key, uint32_t& value) {
    int32_t loaded = 0;
    if (preferences_opt_int32(preferences, key, &loaded)) {
        value = static_cast<uint32_t>(loaded);
    }
}

} // namespace

error_t loadSettings(struct Ili9341PanelConfig& config) {
    const error_t defaults_result = ili9341panel_settings_defaults(&config);
    if (defaults_result != ERROR_NONE) {
        return defaults_result;
    }

    SettingsFile file;
    if (!file.valid()) {
        // No storage yet: run from the defaults.
        return ERROR_NONE;
    }

    Preferences* preferences = file.get();
    loadBool(preferences, "enabled", config.enabled);
    loadInt32(preferences, "spiHost", config.spi_host);
    loadInt32(preferences, "pinSck", config.pin_sck);
    loadInt32(preferences, "pinMosi", config.pin_mosi);
    loadInt32(preferences, "pinMiso", config.pin_miso);
    loadInt32(preferences, "pinDc", config.pin_dc);
    loadInt32(preferences, "pinReset", config.pin_reset);
    loadInt32(preferences, "pinCs", config.pin_cs);
    loadUint16(preferences, "width", config.width);
    loadUint16(preferences, "height", config.height);
    loadBool(preferences, "swapXy", config.swap_xy);
    loadBool(preferences, "mirrorX", config.mirror_x);
    loadBool(preferences, "mirrorY", config.mirror_y);
    loadBool(preferences, "invertColor", config.invert_color);
    loadBool(preferences, "bgrOrder", config.bgr_order);
    loadUint32(preferences, "pixelClockHz", config.pixel_clock_hz);
    loadUint32(preferences, "maxTransferBytes", config.max_transfer_bytes);

    if (config.width == 0 || config.height == 0 || config.pixel_clock_hz == 0 || config.max_transfer_bytes == 0) {
        LOG_W(TAG, "Ignoring invalid saved settings");
        return ili9341panel_settings_defaults(&config);
    }
    return ERROR_NONE;
}

error_t saveSettings(const struct Ili9341PanelConfig& config) {
    SettingsFile file;
    if (!file.valid()) {
        return ERROR_RESOURCE;
    }

    Preferences* preferences = file.get();
    preferences_put_bool(preferences, "enabled", config.enabled);
    preferences_put_int32(preferences, "spiHost", config.spi_host);
    preferences_put_int32(preferences, "pinSck", config.pin_sck);
    preferences_put_int32(preferences, "pinMosi", config.pin_mosi);
    preferences_put_int32(preferences, "pinMiso", config.pin_miso);
    preferences_put_int32(preferences, "pinDc", config.pin_dc);
    preferences_put_int32(preferences, "pinReset", config.pin_reset);
    preferences_put_int32(preferences, "pinCs", config.pin_cs);
    preferences_put_int32(preferences, "width", config.width);
    preferences_put_int32(preferences, "height", config.height);
    preferences_put_bool(preferences, "swapXy", config.swap_xy);
    preferences_put_bool(preferences, "mirrorX", config.mirror_x);
    preferences_put_bool(preferences, "mirrorY", config.mirror_y);
    preferences_put_bool(preferences, "invertColor", config.invert_color);
    preferences_put_bool(preferences, "bgrOrder", config.bgr_order);
    preferences_put_int32(preferences, "pixelClockHz", static_cast<int32_t>(config.pixel_clock_hz));
    preferences_put_int32(preferences, "maxTransferBytes", static_cast<int32_t>(config.max_transfer_bytes));
    return ERROR_NONE;
}

} // namespace ili9341panel

extern "C" error_t ili9341panel_settings_defaults(struct Ili9341PanelConfig* out_config) {
    if (out_config == nullptr) {
        return ERROR_INVALID_ARGUMENT;
    }
    *out_config = Ili9341PanelConfig {
        .enabled = true,
        .spi_host = ili9341panel::DEFAULT_SPI_HOST,
        .pin_sck = 5,
        .pin_mosi = 18,
        .pin_miso = 19,
        .pin_dc = 47,
        .pin_reset = 4,
        .pin_cs = 51,
        .width = 320,
        .height = 240,
        .swap_xy = true,
        .mirror_x = false,
        .mirror_y = false,
        .invert_color = false,
        .bgr_order = true,
        .pixel_clock_hz = 40 * 1000 * 1000,
        .max_transfer_bytes = 16 * 1024,
    };
    return ERROR_NONE;
}

extern "C" error_t ili9341panel_settings_load(struct Ili9341PanelConfig* out_config) {
    if (out_config == nullptr) {
        return ERROR_INVALID_ARGUMENT;
    }
    return ili9341panel::loadSettings(*out_config);
}

extern "C" error_t ili9341panel_settings_save(const struct Ili9341PanelConfig* config) {
    if (config == nullptr) {
        return ERROR_INVALID_ARGUMENT;
    }
    return ili9341panel::saveSettings(*config);
}
