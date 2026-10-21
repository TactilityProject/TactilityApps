// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <webserver.h>

/** Reads the saved settings, or the defaults when there are none. Missing credentials are generated and saved. */
void settingsLoadOrDefault(WebServerSettings& settings);

bool settingsSave(const WebServerSettings& settings);

/** Generates the empty AP password (closed network) and the empty username/password (authentication enabled). @return true when anything was generated */
bool settingsGenerateMissingCredentials(WebServerSettings& settings);
