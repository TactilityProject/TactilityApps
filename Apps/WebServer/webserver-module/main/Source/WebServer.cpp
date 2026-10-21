// SPDX-License-Identifier: GPL-3.0-only
#include "WebServer.h"

#include "Handlers.h"
#include "Settings.h"

#include <tactility/concurrent/mutex.h>
#include <tactility/error.h>
#include <tactility/device.h>
#include <tactility/drivers/wifi.h>
#include <tactility/log.h>

#include <cstring>
#include <iterator>

constexpr auto* TAG = "WebServer";

namespace {

struct State {
    Mutex settingsMutex {};
    WebServerSettings settings {};
    Mutex serverMutex {};
    HttpServer* server = nullptr;
    bool accessPointStarted = false;

    State() {
        mutex_construct(&settingsMutex);
        mutex_construct(&serverMutex);
    }

    ~State() {
        // Not mutex_destruct(), which calls a function that is not available to modules
        vSemaphoreDelete(settingsMutex.handle);
        vSemaphoreDelete(serverMutex.handle);
    }
};

State* state = nullptr;

// Runs the callback with the Wi-Fi device
template<typename Function>
error_t withWifiDevice(Function function) {
    Device* wifi = nullptr;
    if (device_get_first_by_type(&WIFI_TYPE, &wifi) != ERROR_NONE) {
        return ERROR_NOT_FOUND;
    }
    const error_t result = function(wifi);
    device_put(wifi);
    return result;
}

error_t startAccessPoint(const WebServerSettings& settings) {
    WifiAccessPointConfig config {};
    strncpy(config.ssid, settings.ap_ssid, sizeof(config.ssid) - 1);
    if (!settings.ap_open_network) {
        strncpy(config.password, settings.ap_password, sizeof(config.password) - 1);
    }
    config.channel = settings.ap_channel;

    const error_t result = withWifiDevice([&config](Device* wifi) {
        return wifi_access_point_start(wifi, &config);
    });
    if (result != ERROR_NONE) {
        LOG_E(TAG, "Failed to start the access point: %s", error_to_string(result));
    }
    return result;
}

void stopAccessPoint() {
    withWifiDevice([](Device* wifi) {
        return wifi_access_point_stop(wifi);
    });
}

// Must be called with the server mutex held
error_t startServerLocked() {
    if (state->server != nullptr) {
        return ERROR_NONE;
    }

    const WebServerSettings settings = serverGetSettings();

    // In access point mode the server needs the network first
    const bool accessPoint = settings.wifi_mode == WEBSERVER_WIFI_MODE_ACCESS_POINT;
    if (accessPoint && !state->accessPointStarted) {
        if (startAccessPoint(settings) != ERROR_NONE) {
            return ERROR_RESOURCE;
        }
        state->accessPointStarted = true;
    }

    // Dispatchers handle several paths each, because every handler takes a URI slot of the HTTP server
    static const HttpServerRequestHandler handlers[] = {
        { "/", HTTP_METHOD_GET, handleRoot, nullptr },
        { "/filebrowser", HTTP_METHOD_GET, handleFileBrowser, nullptr },
        { "/fs/*", HTTP_METHOD_GET, handleFsGet, nullptr },
        { "/fs/*", HTTP_METHOD_POST, handleFsPost, nullptr },
        { "/admin/*", HTTP_METHOD_POST, handleAdminPost, nullptr },
        { "/api/*", HTTP_METHOD_GET, handleApiGet, nullptr },
        { "/api/*", HTTP_METHOD_POST, handleApiPost, nullptr },
        { "/api/*", HTTP_METHOD_PUT, handleApiPut, nullptr },
        { "/*", HTTP_METHOD_GET, handleAssets, nullptr },
    };

    HttpServerConfig config {
        .port = settings.port,
        .address = "0.0.0.0",
        .stack_size = 8192,
        .handlers = handlers,
        .handler_count = std::size(handlers),
    };

    state->server = http_server_alloc(&config);
    if (state->server == nullptr || http_server_start(state->server) != ERROR_NONE) {
        LOG_E(TAG, "Failed to start the HTTP server on port %u", static_cast<unsigned>(settings.port));
        http_server_free(state->server);
        state->server = nullptr;
        if (state->accessPointStarted) {
            stopAccessPoint();
            state->accessPointStarted = false;
        }
        return ERROR_RESOURCE;
    }

    LOG_I(TAG, "HTTP server started on port %u", static_cast<unsigned>(settings.port));
    return ERROR_NONE;
}

// Must be called with the server mutex held
void stopServerLocked() {
    if (state->server == nullptr) {
        return;
    }
    http_server_free(state->server);
    state->server = nullptr;
    if (state->accessPointStarted) {
        stopAccessPoint();
        state->accessPointStarted = false;
    }
    LOG_I(TAG, "HTTP server stopped");
}

}

void serverStart() {
    state = new State();

    settingsLoadOrDefault(state->settings);

    if (state->settings.enabled) {
        serverSetRunning(true);
    } else {
        LOG_I(TAG, "The web server is disabled in the settings");
    }
}

void serverStop() {
    serverSetRunning(false);

    delete state;
    state = nullptr;
}

bool serverIsRunning() {
    mutex_lock(&state->serverMutex);
    const bool running = state->server != nullptr;
    mutex_unlock(&state->serverMutex);
    return running;
}

error_t serverSetRunning(bool running) {
    mutex_lock(&state->serverMutex);
    error_t error = ERROR_NONE;
    if (running) {
        error = startServerLocked();
    } else {
        stopServerLocked();
    }
    mutex_unlock(&state->serverMutex);
    return error;
}

WebServerSettings serverGetSettings() {
    mutex_lock(&state->settingsMutex);
    const WebServerSettings copy = state->settings;
    mutex_unlock(&state->settingsMutex);
    return copy;
}

error_t serverSaveSettings(const WebServerSettings& settings) {
    // E.g. authentication was enabled with empty credentials
    WebServerSettings completed = settings;
    settingsGenerateMissingCredentials(completed);
    // A closed access point can't start with a password outside the WPA2 limits.
    // Rejected instead of saved, or the server would fail to start on every boot.
    if (!completed.ap_open_network) {
        const size_t length = strlen(completed.ap_password);
        if (length < WEBSERVER_AP_PASSWORD_MIN_LENGTH || length > WEBSERVER_AP_PASSWORD_MAX_LENGTH) {
            LOG_E(TAG, "Invalid access point password length: %u", static_cast<unsigned>(length));
            return ERROR_INVALID_ARGUMENT;
        }
    }
    if (!settingsSave(completed)) {
        return ERROR_RESOURCE;
    }
    mutex_lock(&state->settingsMutex);
    state->settings = completed;
    mutex_unlock(&state->settingsMutex);
    return ERROR_NONE;
}
