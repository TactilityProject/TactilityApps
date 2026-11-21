// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <tactility/error.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** What the display shows while it is idle */
enum ScreensaverType {
    /** A black screen */
    SCREENSAVER_TYPE_NONE = 0,
    SCREENSAVER_TYPE_BOUNCING_BALLS = 1,
    SCREENSAVER_TYPE_MYSTIFY = 2,
    SCREENSAVER_TYPE_MATRIX_RAIN = 3,
    SCREENSAVER_TYPE_STACKCHAN = 4,
    /** Not a type, the number of types */
    SCREENSAVER_TYPE_COUNT = 5,
};

/** The settings of the screen timeout and the screensaver */
struct ScreensaverSettings {
    /** Start the screensaver when the display was idle for timeout_ms */
    bool enabled;
    uint32_t timeout_ms;
    /** A ScreensaverType */
    uint8_t type;
};

/** @return the settings that the module uses */
error_t screensaver_settings_load(struct ScreensaverSettings* settings);

/** Stores the settings and applies them */
error_t screensaver_settings_save(const struct ScreensaverSettings* settings);

/** Shows the screensaver of the settings now, until the screen is touched */
error_t screensaver_preview(void);

#ifdef __cplusplus
}
#endif
