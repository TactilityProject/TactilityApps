// SPDX-License-Identifier: GPL-3.0-only
#include "Settings.h"

#include <app/paths.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr auto* APP_ID = "tactility.screensaver";
constexpr auto* FILE_NAME = "screensaver.properties";

constexpr auto* KEY_ENABLED = "enabled";
constexpr auto* KEY_TIMEOUT_MS = "timeoutMs";
constexpr auto* KEY_TYPE = "type";

constexpr uint32_t DEFAULT_TIMEOUT_MS = 60000;

bool getPath(char* path, size_t size) {
    return app_paths_get_user_data_path(APP_ID, FILE_NAME, path, size) == ERROR_NONE;
}

// A table of pointers isn't relocated when the module is loaded, so the names are in a switch
const char* typeToName(uint8_t type) {
    switch (type) {
        case SCREENSAVER_TYPE_NONE: return "none";
        case SCREENSAVER_TYPE_MYSTIFY: return "mystify";
        case SCREENSAVER_TYPE_MATRIX_RAIN: return "matrixRain";
        case SCREENSAVER_TYPE_STACKCHAN: return "stackChan";
        default: return "bouncingBalls";
    }
}

uint8_t parseType(const char* value) {
    for (uint8_t i = 0; i < SCREENSAVER_TYPE_COUNT; i++) {
        if (strcmp(value, typeToName(i)) == 0) {
            return i;
        }
    }
    return SCREENSAVER_TYPE_BOUNCING_BALLS;
}

}

void settingsLoadOrDefault(ScreensaverSettings& settings) {
    settings = {
        .enabled = false,
        .timeout_ms = DEFAULT_TIMEOUT_MS,
        .type = SCREENSAVER_TYPE_BOUNCING_BALLS,
    };

    char path[256];
    if (!getPath(path, sizeof(path))) {
        return;
    }
    FILE* file = fopen(path, "r");
    if (file == nullptr) {
        return;
    }
    char line[96];
    while (fgets(line, sizeof(line), file) != nullptr) {
        line[strcspn(line, "\r\n")] = '\0';
        char* value = strchr(line, '=');
        if (value == nullptr) {
            continue;
        }
        *value++ = '\0';
        if (strcmp(line, KEY_ENABLED) == 0) {
            settings.enabled = strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
        } else if (strcmp(line, KEY_TIMEOUT_MS) == 0) {
            const long timeout = strtol(value, nullptr, 10);
            if (timeout > 0) {
                settings.timeout_ms = static_cast<uint32_t>(timeout);
            }
        } else if (strcmp(line, KEY_TYPE) == 0) {
            settings.type = parseType(value);
        }
    }
    fclose(file);
}

bool settingsSave(const ScreensaverSettings& settings) {
    char path[256];
    if (!getPath(path, sizeof(path))) {
        return false;
    }
    FILE* file = fopen(path, "w");
    if (file == nullptr) {
        return false;
    }
    const bool written = fprintf(file, "%s=%d\n%s=%lu\n%s=%s\n",
        KEY_ENABLED, settings.enabled ? 1 : 0,
        KEY_TIMEOUT_MS, static_cast<unsigned long>(settings.timeout_ms),
        KEY_TYPE, typeToName(settings.type)) > 0;
    return fclose(file) == 0 && written;
}
