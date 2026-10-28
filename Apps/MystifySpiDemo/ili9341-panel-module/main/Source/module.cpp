// SPDX-License-Identifier: Apache-2.0
#include "Panel.h"
#include "Settings.h"

#include "Ili9341Spi.h"
#include <ili9341panel.h>

#include <tactility/device.h>
#include <tactility/driver.h>
#include <tactility/error.h>
#include <tactility/log.h>
#include <tactility/module.h>

constexpr auto* TAG = "Ili9341PanelModule";

extern "C" {

/**
 * The driver is registered manually instead of through Module.drivers: the kernel calls start()
 * *before* it constructs the drivers in that list, and the device can only be created afterwards.
 */
static error_t start() {
    if (driver_construct_add(&ili9341_spi_driver) != ERROR_NONE) {
        LOG_E(TAG, "Could not register the ILI9341 driver");
        return ERROR_RESOURCE;
    }

    struct Ili9341PanelConfig config {};
    error_t settings_result = ili9341panel_settings_load(&config);
    if (settings_result != ERROR_NONE) {
        LOG_E(TAG, "Could not load settings; using defaults");
        ili9341panel_settings_defaults(&config);
    }

    // A failure here leaves the module running so the settings app can fix the configuration.
    if (ili9341panel::startPanel(config) != ERROR_NONE) {
        LOG_E(TAG, "Panel not started; check the saved settings");
    }
    return ERROR_NONE;
}

static error_t stop() {
    error_t panel_result = ili9341panel::stopPanel();
    if (panel_result != ERROR_NONE) {
        // Keep the driver registered: the device (and its driver) is still in use.
        return panel_result;
    }
    if (driver_remove_destruct(&ili9341_spi_driver) != ERROR_NONE) {
        return ERROR_RESOURCE;
    }
    return ERROR_NONE;
}

error_t ili9341panel_apply() {
    struct Ili9341PanelConfig config {};
    error_t result = ili9341panel_settings_load(&config);
    if (result != ERROR_NONE) {
        return result;
    }
    error_t stop_result = ili9341panel::stopPanel();
    if (stop_result != ERROR_NONE) {
        return stop_result;
    }
    return ili9341panel::startPanel(config);
}

bool ili9341panel_is_active() {
    return ili9341panel::isPanelActive();
}

error_t ili9341panel_get_device(struct Device** out_device) {
    if (out_device == nullptr) {
        return ERROR_INVALID_ARGUMENT;
    }
    struct Device* device = ili9341panel::getPanelDevice();
    if (device == nullptr) {
        return ERROR_NOT_FOUND;
    }
    *out_device = device;
    return ERROR_NONE;
}

static const ModuleSymbol symbols[] = {
    DEFINE_MODULE_SYMBOL(ili9341panel_settings_defaults),
    DEFINE_MODULE_SYMBOL(ili9341panel_settings_load),
    DEFINE_MODULE_SYMBOL(ili9341panel_settings_save),
    DEFINE_MODULE_SYMBOL(ili9341panel_apply),
    DEFINE_MODULE_SYMBOL(ili9341panel_is_active),
    DEFINE_MODULE_SYMBOL(ili9341panel_get_device),
    MODULE_SYMBOL_TERMINATOR
};

static Module module = {
    .name = "ili9341-panel",
    .start = start,
    .stop = stop,
    .drivers = nullptr,
    .symbols = symbols,
    .internal = nullptr,
};

// The entry point of a module returns its struct Module. The asm label keeps the symbol name of
// main for another return type.
#define ENTRY_NAME_INNER(name) #name
#define ENTRY_NAME(name) ENTRY_NAME_INNER(name)
Module* module_main(int argc, char* argv[]) __asm__(ENTRY_NAME(main));

Module* module_main(int argc, char* argv[]) {
    return &module;
}

}
