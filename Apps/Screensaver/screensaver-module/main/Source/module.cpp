// SPDX-License-Identifier: GPL-3.0-only
#include "Idle.h"
#include "Settings.h"

#include <screensaver.h>

#include <tactility/error.h>
#include <tactility/module.h>

extern "C" {

static error_t start() {
    ScreensaverSettings settings;
    settingsLoadOrDefault(settings);
    return idleStart(settings) ? ERROR_NONE : ERROR_INVALID_STATE;
}

static error_t stop() {
    idleStop();
    return ERROR_NONE;
}

error_t screensaver_settings_load(ScreensaverSettings* settings) {
    *settings = idleGetSettings();
    return ERROR_NONE;
}

error_t screensaver_settings_save(const ScreensaverSettings* settings) {
    if (settings->type >= SCREENSAVER_TYPE_COUNT) {
        return ERROR_INVALID_ARGUMENT;
    }
    idleApplySettings(*settings);
    return settingsSave(*settings) ? ERROR_NONE : ERROR_RESOURCE;
}

error_t screensaver_preview() {
    idlePreview();
    return ERROR_NONE;
}

static const ModuleSymbol symbols[] = {
    DEFINE_MODULE_SYMBOL(screensaver_settings_load),
    DEFINE_MODULE_SYMBOL(screensaver_settings_save),
    DEFINE_MODULE_SYMBOL(screensaver_preview),
    MODULE_SYMBOL_TERMINATOR
};

static Module module = {
    .name = "screensaver",
    .start = start,
    .stop = stop,
    .drivers = nullptr,
    .symbols = symbols,
    .internal = nullptr,
};

// The entry point of a module returns its struct Module. The asm label keeps the symbol name of main for another return type.
// The build renames main to app_main for the ELF loader, which is why the label goes through a macro.
#define ENTRY_NAME_INNER(name) #name
#define ENTRY_NAME(name) ENTRY_NAME_INNER(name)
Module* module_main(int argc, char* argv[]) __asm__(ENTRY_NAME(main));

Module* module_main(int argc, char* argv[]) {
    return &module;
}

}
