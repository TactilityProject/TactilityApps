// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <screensaver.h>

/** Reads the saved settings, or the defaults when there are none */
void settingsLoadOrDefault(ScreensaverSettings& settings);

bool settingsSave(const ScreensaverSettings& settings);
