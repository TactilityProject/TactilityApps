// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <webserver.h>

/** Reads the saved settings, or the defaults when there are none. Empty credentials are generated while authentication is enabled, but not saved. */
void settingsLoadOrDefault(WebServerSettings& settings);

bool settingsSave(const WebServerSettings& settings);
