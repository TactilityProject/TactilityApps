// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <webserver.h>

/** Loads the settings and starts the server when it is enabled in the settings */
void serverStart();

/** Stops the server */
void serverStop();

bool serverIsRunning();

error_t serverSetRunning(bool running);

/** A copy of the settings that the server uses for new requests */
WebServerSettings serverGetSettings();

/** Saves the settings and makes the server use them for new requests */
error_t serverSaveSettings(const WebServerSettings& settings);
