// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <screensaver.h>

/** Starts watching the display for inactivity. @return false when LVGL isn't running. */
bool idleStart(const ScreensaverSettings& settings);

/** Stops the screensaver and watching */
void idleStop();

/** Uses new settings from the next check on */
void idleApplySettings(const ScreensaverSettings& settings);

/** Shows the screensaver until the screen is touched */
void idlePreview();

/** @return the settings in use */
ScreensaverSettings idleGetSettings();
