// SPDX-License-Identifier: GPL-3.0-only
#include "Settings.h"

#include "Util.h"

#include <app/paths.h>
#include <tactility/log.h>

#include <esp_mac.h>
#include <esp_random.h>

#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <map>
#include <string>

constexpr auto* TAG = "WebServerSettings";

constexpr auto* APP_ID = "tactility.webserver";
constexpr auto* FILE_NAME = "webserver.properties";

constexpr auto* KEY_ENABLED = "webServerEnabled";
constexpr auto* KEY_PORT = "webServerPort";
constexpr auto* KEY_AUTH_ENABLED = "webServerAuthEnabled";
constexpr auto* KEY_USERNAME = "webServerUsername";
constexpr auto* KEY_PASSWORD = "webServerPassword";
constexpr auto* KEY_WIFI_MODE = "wifiMode";
constexpr auto* KEY_AP_SSID = "apSsid";
constexpr auto* KEY_AP_PASSWORD = "apPassword";
constexpr auto* KEY_AP_OPEN_NETWORK = "apOpenNetwork";
constexpr auto* KEY_AP_CHANNEL = "apChannel";

namespace {

std::string getSettingsPath() {
    char path[256];
    if (app_paths_get_user_data_path(APP_ID, FILE_NAME, path, sizeof(path)) != ERROR_NONE) {
        return "";
    }
    return path;
}

std::string trim(const std::string& text) {
    constexpr auto* whitespace = " \t\r\n";
    const size_t start = text.find_first_not_of(whitespace);
    if (start == std::string::npos) {
        return "";
    }
    return text.substr(start, text.find_last_not_of(whitespace) - start + 1);
}

bool loadProperties(const std::string& path, std::map<std::string, std::string>& properties) {
    FILE* file = fopen(path.c_str(), "r");
    if (file == nullptr) {
        return false;
    }
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), file) != nullptr) {
        const std::string trimmed = trim(buffer);
        if (trimmed.empty() || trimmed.starts_with("#")) {
            continue;
        }
        const size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            continue;
        }
        properties[trim(trimmed.substr(0, separator))] = trim(trimmed.substr(separator + 1));
    }
    fclose(file);
    return true;
}

void copyString(char* destination, size_t destinationSize, const std::string& source) {
    strncpy(destination, source.c_str(), destinationSize - 1);
    destination[destinationSize - 1] = '\0';
}

bool parseBool(const std::map<std::string, std::string>& properties, const char* key) {
    const auto iterator = properties.find(key);
    return iterator != properties.end() && (iterator->second == "1" || iterator->second == "true");
}

std::string generateCredential(size_t length) {
    static constexpr char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string result;
    result.reserve(length);
    for (size_t i = 0; i < length; ++i) {
        result += charset[esp_random() % (sizeof(charset) - 1)];
    }
    return result;
}

// Unique per device. The base MAC exists on every chip, also on those without Wi-Fi of their own.
std::string generateDefaultSsid() {
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        return "Tactility-0000";
    }
    return util::format("Tactility-%02X%02X", mac[2] ^ mac[3], mac[4] ^ mac[5]);
}

int parseInteger(const std::map<std::string, std::string>& properties, const char* key, int minimum, int maximum, int fallback) {
    const auto iterator = properties.find(key);
    if (iterator == properties.end()) {
        return fallback;
    }
    char* end = nullptr;
    const long value = strtol(iterator->second.c_str(), &end, 10);
    if (end == iterator->second.c_str() || *end != '\0' || value < minimum || value > maximum) {
        return fallback;
    }
    return static_cast<int>(value);
}


void getDefault(WebServerSettings& settings) {
    memset(&settings, 0, sizeof(settings));
    settings.port = WEBSERVER_DEFAULT_PORT;
    settings.wifi_mode = WEBSERVER_WIFI_MODE_STATION;
    copyString(settings.ap_ssid, sizeof(settings.ap_ssid), generateDefaultSsid());
    settings.ap_channel = 1;
}

}

// Only the empty credentials are generated, the ones the user typed are kept
bool settingsGenerateMissingCredentials(WebServerSettings& settings) {
    bool generated = false;
    if (!settings.ap_open_network && settings.ap_password[0] == '\0') {
        LOG_I(TAG, "The access point has no password: generating one");
        copyString(settings.ap_password, sizeof(settings.ap_password), generateCredential(12));
        generated = true;
    }
    if (settings.auth_enabled && (settings.username[0] == '\0' || settings.password[0] == '\0')) {
        LOG_I(TAG, "Authentication is enabled without credentials: generating them");
        copyString(settings.username, sizeof(settings.username), generateCredential(12));
        copyString(settings.password, sizeof(settings.password), generateCredential(12));
        generated = true;
    }
    return generated;
}

void settingsLoadOrDefault(WebServerSettings& settings) {
    getDefault(settings);

    const std::string path = getSettingsPath();
    std::map<std::string, std::string> properties;
    if (path.empty() || !util::isFile(path) || !loadProperties(path, properties)) {
        // Saved, so every later load returns the same credentials
        if (settingsGenerateMissingCredentials(settings)) settingsSave(settings);
        return;
    }

    settings.enabled = parseBool(properties, KEY_ENABLED);
    settings.auth_enabled = parseBool(properties, KEY_AUTH_ENABLED);

    settings.port = static_cast<uint16_t>(parseInteger(properties, KEY_PORT, 1, 65535, WEBSERVER_DEFAULT_PORT));
    settings.wifi_mode = parseInteger(properties, KEY_WIFI_MODE, 0, 1, WEBSERVER_WIFI_MODE_STATION) == 1
        ? WEBSERVER_WIFI_MODE_ACCESS_POINT
        : WEBSERVER_WIFI_MODE_STATION;
    settings.ap_channel = static_cast<uint8_t>(parseInteger(properties, KEY_AP_CHANNEL, 1, WEBSERVER_AP_MAX_CHANNEL, 1));
    settings.ap_open_network = parseBool(properties, KEY_AP_OPEN_NETWORK);

    const auto apSsid = properties.find(KEY_AP_SSID);
    if (apSsid != properties.end() && !apSsid->second.empty()) {
        copyString(settings.ap_ssid, sizeof(settings.ap_ssid), apSsid->second);
    }
    const auto apPassword = properties.find(KEY_AP_PASSWORD);
    if (apPassword != properties.end()) {
        copyString(settings.ap_password, sizeof(settings.ap_password), apPassword->second);
    }

    const auto username = properties.find(KEY_USERNAME);
    if (username != properties.end()) {
        copyString(settings.username, sizeof(settings.username), username->second);
    }
    const auto password = properties.find(KEY_PASSWORD);
    if (password != properties.end()) {
        copyString(settings.password, sizeof(settings.password), password->second);
    }

    // Saved, so every later load returns the same credentials
    if (settingsGenerateMissingCredentials(settings)) settingsSave(settings);
}

bool settingsSave(const WebServerSettings& settings) {
    const std::string path = getSettingsPath();
    if (path.empty() || !util::findOrCreateParentDirectory(path, 0755)) {
        LOG_E(TAG, "Failed to create the settings directory");
        return false;
    }

    FILE* file = fopen(path.c_str(), "w");
    if (file == nullptr) {
        LOG_E(TAG, "Failed to open %s", path.c_str());
        return false;
    }
    int written = fprintf(file, "%s=%s\n", KEY_ENABLED, settings.enabled ? "1" : "0");
    written += fprintf(file, "%s=%u\n", KEY_PORT, static_cast<unsigned>(settings.port));
    written += fprintf(file, "%s=%s\n", KEY_AUTH_ENABLED, settings.auth_enabled ? "1" : "0");
    written += fprintf(file, "%s=%s\n", KEY_USERNAME, settings.username);
    written += fprintf(file, "%s=%s\n", KEY_PASSWORD, settings.password);
    written += fprintf(file, "%s=%u\n", KEY_WIFI_MODE, static_cast<unsigned>(settings.wifi_mode));
    written += fprintf(file, "%s=%s\n", KEY_AP_SSID, settings.ap_ssid);
    written += fprintf(file, "%s=%s\n", KEY_AP_PASSWORD, settings.ap_password);
    written += fprintf(file, "%s=%s\n", KEY_AP_OPEN_NETWORK, settings.ap_open_network ? "1" : "0");
    written += fprintf(file, "%s=%u\n", KEY_AP_CHANNEL, static_cast<unsigned>(settings.ap_channel));
    const bool success = fclose(file) == 0 && written > 0;
    if (!success) {
        LOG_E(TAG, "Failed to write %s", path.c_str());
    }
    return success;
}
