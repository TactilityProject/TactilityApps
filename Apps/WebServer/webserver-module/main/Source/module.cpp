// SPDX-License-Identifier: GPL-3.0-only
#include "WebServer.h"

#include <webserver.h>

#include <tactility/error.h>
#include <tactility/module.h>

#include "Settings.h"

extern "C" {

static error_t start() {
    serverStart();
    return ERROR_NONE;
}

static error_t stop() {
    serverStop();
    return ERROR_NONE;
}

error_t webserver_settings_load(WebServerSettings* settings) {
    // The active settings, not a fresh load: that would generate new (unsaved) random credentials
    *settings = serverGetSettings();
    return ERROR_NONE;
}

error_t webserver_settings_save(const WebServerSettings* settings) {
    return serverSaveSettings(*settings);
}

bool webserver_is_enabled() {
    return serverIsRunning();
}

error_t webserver_set_enabled(bool enabled) {
    return serverSetRunning(enabled);
}

static const ModuleSymbol symbols[] = {
    DEFINE_MODULE_SYMBOL(webserver_settings_load),
    DEFINE_MODULE_SYMBOL(webserver_settings_save),
    DEFINE_MODULE_SYMBOL(webserver_is_enabled),
    DEFINE_MODULE_SYMBOL(webserver_set_enabled),
    MODULE_SYMBOL_TERMINATOR
};

static Module module = {
    .name = "webserver",
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
