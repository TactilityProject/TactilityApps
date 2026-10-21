// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <tactility/error.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEBSERVER_USERNAME_MAX_LENGTH 32
#define WEBSERVER_PASSWORD_MAX_LENGTH 64
#define WEBSERVER_DEFAULT_PORT 80
#define WEBSERVER_SSID_MAX_LENGTH 32
#define WEBSERVER_AP_PASSWORD_MIN_LENGTH 8
#define WEBSERVER_AP_PASSWORD_MAX_LENGTH 63
#define WEBSERVER_AP_PASSWORD_BUFFER_LENGTH 64
#define WEBSERVER_AP_MAX_CHANNEL 13

/** How the device is reachable */
enum WebServerWifiMode {
    /** Over the Wi-Fi network that the device is connected to */
    WEBSERVER_WIFI_MODE_STATION = 0,
    /** The device is a Wi-Fi network itself, at 192.168.4.1 */
    WEBSERVER_WIFI_MODE_ACCESS_POINT = 1,
};

/** The settings of the web server. All strings are NUL-terminated. */
struct WebServerSettings {
    /** Start the server when the module starts */
    bool enabled;
    uint16_t port;
    /** Require HTTP basic authentication */
    bool auth_enabled;
    char username[WEBSERVER_USERNAME_MAX_LENGTH + 1];
    char password[WEBSERVER_PASSWORD_MAX_LENGTH + 1];
    /** A WebServerWifiMode */
    uint8_t wifi_mode;
    /** The name of the Wi-Fi network in access point mode */
    char ap_ssid[WEBSERVER_SSID_MAX_LENGTH + 1];
    /** The password of the Wi-Fi network in access point mode. It needs 8 to 63 characters unless the network is open. */
    char ap_password[WEBSERVER_AP_PASSWORD_BUFFER_LENGTH];
    /** Access point mode without a password */
    bool ap_open_network;
    /** The Wi-Fi channel from 1 to 13 */
    uint8_t ap_channel;
};

/**
 * Reads the settings, or the defaults when there are none yet.
 * Credentials that are empty while authentication is enabled are generated, but not saved.
 */
error_t webserver_settings_load(struct WebServerSettings* settings);

/**
 * Saves the settings and makes the running server use them for new requests.
 * The port and the access point settings apply when the server starts the next time.
 */
error_t webserver_settings_save(const struct WebServerSettings* settings);

/** @return true while the HTTP server is running */
bool webserver_is_enabled(void);

/**
 * Starts or stops the HTTP server. The saved settings are not changed.
 * @retval ERROR_RESOURCE the server could not be started
 */
error_t webserver_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
