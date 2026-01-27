#include "FTPServer.h"

#include <tt_app.h>
#include <tt_hal.h>
#include <tt_lvgl.h>
#include <tt_lvgl_toolbar.h>
#include <tt_wifi.h>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "FtpServerCore.h"
#include <Tactility/kernel/Kernel.h>
#include <lwip/ip4_addr.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

constexpr auto* TAG = "FTPServer";

#define WIFI_STATE_CONNECTION_ACTIVE 3

#define SETTINGS_FILENAME "ftpserver.properties"

// Default credentials
#define DEFAULT_FTP_USER "esp32"
#define DEFAULT_FTP_PASS "esp32"
#define DEFAULT_FTP_PORT 21

// FTP Server instance (static because FtpServerCore expects it)
static FtpServer::Server* ftpServer = nullptr;

// Settings (static for persistence across view switches)
static char ftpUsername[33] = DEFAULT_FTP_USER;
static char ftpPassword[33] = DEFAULT_FTP_PASS;
static int ftpPort = DEFAULT_FTP_PORT;

constexpr auto* KEY_FTPSERVER_USER = "username";
constexpr auto* KEY_FTPSERVER_PASS = "password";
constexpr auto* KEY_FTPSERVER_PASS_ENC = "password_enc";
constexpr auto* KEY_FTPSERVER_PORT = "port";

// Simple XOR key for password obfuscation (not cryptographically secure, but prevents casual reading)
static constexpr uint8_t XOR_KEY[] = {0x5A, 0x3C, 0x7E, 0x1D, 0x9B, 0x4F, 0x2A, 0x6E};
static constexpr size_t XOR_KEY_LEN = sizeof(XOR_KEY);

// Encode password to hex string with XOR obfuscation
static void encodePassword(const char* plain, char* encoded, size_t encodedSize) {
    size_t len = strlen(plain);
    size_t outIdx = 0;

    for (size_t i = 0; i < len && outIdx + 2 < encodedSize; i++) {
        uint8_t obfuscated = static_cast<uint8_t>(plain[i]) ^ XOR_KEY[i % XOR_KEY_LEN];
        snprintf(encoded + outIdx, 3, "%02X", obfuscated);
        outIdx += 2;
    }
    encoded[outIdx] = '\0';
}

// Decode hex string with XOR obfuscation back to password
static bool decodePassword(const char* encoded, char* plain, size_t plainSize) {
    size_t len = strlen(encoded);
    if (len % 2 != 0) {
        return false;
    }
    if ((len / 2) >= plainSize) {
        return false;
    }

    size_t outIdx = 0;
    for (size_t i = 0; i < len && outIdx + 1 < plainSize; i += 2) {
        unsigned int byte;
        if (sscanf(encoded + i, "%02X", &byte) != 1) {
            return false;
        }
        plain[outIdx] = static_cast<char>(static_cast<uint8_t>(byte) ^ XOR_KEY[outIdx % XOR_KEY_LEN]);
        outIdx++;
    }
    plain[outIdx] = '\0';
    return true;
}

// App handle for settings path
static AppHandle currentAppHandle = nullptr;

// Pointer to current app instance for log callback
static FTPServer* currentInstance = nullptr;

//====================================================================================================
// Settings persistence
//====================================================================================================
// TODO: Replace these functions with loadPropertiesFiles / savePropertiesFile when available to apps
static bool mkdirRecursive(const char* path) {
    char tmp[256];
    char* p = nullptr;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);

    if (len > 0 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            struct stat st;
            if (stat(tmp, &st) != 0) {
                int ret = mkdir(tmp, 0755);
                if (ret != 0 && errno != EEXIST) {
                    ESP_LOGE(TAG, "Failed to create directory: %s (errno=%d)", tmp, errno);
                    return false;
                }
            }
            *p = '/';
        }
    }

    struct stat st;
    if (stat(tmp, &st) != 0) {
        int ret = mkdir(tmp, 0755);
        if (ret != 0 && errno != EEXIST) {
            ESP_LOGE(TAG, "Failed to create final directory: %s (errno=%d)", tmp, errno);
            return false;
        }
    }

    return true;
}

static bool getSettingsPath(char* buffer, size_t bufferSize) {
    if (!currentAppHandle) {
        return false;
    }

    size_t size = bufferSize;
    tt_app_get_user_data_child_path(currentAppHandle, SETTINGS_FILENAME, buffer, &size);

    if (size == 0) {
        ESP_LOGE(TAG, "Failed to get user data path");
        return false;
    }

    char dirBuffer[256];
    size_t dirSize = sizeof(dirBuffer);
    tt_app_get_user_data_path(currentAppHandle, dirBuffer, &dirSize);

    if (dirSize > 0) {
        struct stat st;
        if (stat(dirBuffer, &st) != 0) {
            if (!mkdirRecursive(dirBuffer)) {
                ESP_LOGE(TAG, "Failed to create settings directory: %s", dirBuffer);
                return false;
            }
        }
    }

    return true;
}

static bool saveSettings() {
    char path[256];
    if (!getSettingsPath(path, sizeof(path))) {
        return false;
    }

    FILE* file = fopen(path, "w");
    if (file != nullptr) {
        fprintf(file, "%s=%s\n", KEY_FTPSERVER_USER, ftpUsername);
        // Store password as obfuscated hex instead of plaintext
        char encodedPass[128];
        encodePassword(ftpPassword, encodedPass, sizeof(encodedPass));
        fprintf(file, "%s=%s\n", KEY_FTPSERVER_PASS_ENC, encodedPass);
        fprintf(file, "%s=%d\n", KEY_FTPSERVER_PORT, ftpPort);
        fclose(file);
        ESP_LOGI(TAG, "Settings saved to %s", path);
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to open %s for writing", path);
        return false;
    }
}

static bool loadSettings() {
    char path[256];
    if (!getSettingsPath(path, sizeof(path))) {
        return false;
    }

    FILE* file = fopen(path, "r");
    if (file == nullptr) {
        ESP_LOGI(TAG, "No settings file found, using defaults");
        return false;
    }

    bool foundPlaintextPassword = false;
    bool foundEncodedPassword = false;

    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '\0' || line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }

        char* eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            char* key = line;
            char* value = eq + 1;

            char* nl = strchr(value, '\n');
            if (nl) *nl = '\0';
            char* cr = strchr(value, '\r');
            if (cr) *cr = '\0';

            if (strcmp(key, KEY_FTPSERVER_USER) == 0) {
                strncpy(ftpUsername, value, sizeof(ftpUsername) - 1);
                ftpUsername[sizeof(ftpUsername) - 1] = '\0';
            } else if (strcmp(key, KEY_FTPSERVER_PASS_ENC) == 0) {
                // Decode obfuscated password
                if (decodePassword(value, ftpPassword, sizeof(ftpPassword))) {
                    foundEncodedPassword = true;
                }
            } else if (strcmp(key, KEY_FTPSERVER_PASS) == 0) {
                // Legacy plaintext password - migrate it
                if (!foundEncodedPassword) {
                    strncpy(ftpPassword, value, sizeof(ftpPassword) - 1);
                    ftpPassword[sizeof(ftpPassword) - 1] = '\0';
                    foundPlaintextPassword = true;
                }
            } else if (strcmp(key, KEY_FTPSERVER_PORT) == 0) {
                int port = atoi(value);
                if (port > 0 && port <= 65535) {
                    ftpPort = port;
                }
            }
        }
    }
    fclose(file);

    // Migrate: if we found a plaintext password, re-save with encoded password
    if (foundPlaintextPassword && !foundEncodedPassword) {
        ESP_LOGI(TAG, "Migrating plaintext password to encoded format");
        saveSettings();
    }

    ESP_LOGI(TAG, "Settings loaded: user=%s, port=%d", ftpUsername, ftpPort);
    return true;
}

//==============================================================================================
// UI Helpers
//==============================================================================================

static bool isWifiConnected() {
    WifiRadioState state = tt_wifi_get_radio_state();
    return state == WIFI_STATE_CONNECTION_ACTIVE;
}

//==============================================================================================
// FTPServer View Management
//==============================================================================================

void FTPServer::stopActiveView() {
    if (activeView != nullptr) {
        activeView->onStop();
        lv_obj_clean(wrapperWidget);
        activeView = nullptr;
    }
}

void FTPServer::showMainView() {
    ESP_LOGI(TAG, "showMainView");
    stopActiveView();
    activeView = &mainView;
    mainView.onStart(wrapperWidget);

    // Show toolbar items
    if (settingsButton) {
        lv_obj_remove_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);
    }
    if (connectSwitch) {
        lv_obj_remove_flag(connectSwitch, LV_OBJ_FLAG_HIDDEN);
    }
    if (clearLogButton) {
        lv_obj_remove_flag(clearLogButton, LV_OBJ_FLAG_HIDDEN);
    }

    // Update UI based on server state
    if (ftpServer && ftpServer->isEnabled()) {
        if (spinner) {
            lv_obj_remove_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        }

        char ipStr[32] = "IP: --";
        if (isWifiConnected()) {
            esp_netif_ip_info_t ipInfo;
            if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ipInfo) == ESP_OK) {
                snprintf(ipStr, sizeof(ipStr), "IP: " IPSTR, IP2STR(&ipInfo.ip));
            }
        }
        mainView.updateInfoPanel(ipStr, "Running", LV_PALETTE_GREEN);
    } else {
        if (spinner) {
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void FTPServer::showSettingsView() {
    ESP_LOGI(TAG, "showSettingsView");
    stopActiveView();
    activeView = &settingsView;
    settingsView.onStart(wrapperWidget, ftpUsername, ftpPassword, ftpPort);

    // Hide toolbar items when in settings
    if (settingsButton) {
        lv_obj_add_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);
    }
    if (connectSwitch) {
        lv_obj_add_flag(connectSwitch, LV_OBJ_FLAG_HIDDEN);
    }
    if (spinner) {
        lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
    }
    if (clearLogButton) {
        lv_obj_add_flag(clearLogButton, LV_OBJ_FLAG_HIDDEN);
    }
}

void FTPServer::onSettingsSaved(const char* username, const char* password, int port) {
    if (username && strlen(username) > 0) {
        strncpy(ftpUsername, username, sizeof(ftpUsername) - 1);
        ftpUsername[sizeof(ftpUsername) - 1] = '\0';
    }

    if (password && strlen(password) > 0) {
        strncpy(ftpPassword, password, sizeof(ftpPassword) - 1);
        ftpPassword[sizeof(ftpPassword) - 1] = '\0';
    }

    if (port > 0 && port <= 65535) {
        ftpPort = port;
    }

    if (ftpServer) {
        ftpServer->setCredentials(ftpUsername, ftpPassword);
        ftpServer->setPort(static_cast<uint16_t>(ftpPort));
    }

    saveSettings();
    ESP_LOGI(TAG, "Settings updated: user=%s, port=%d", ftpUsername, ftpPort);

    showMainView();
}

void FTPServer::onSettingsButtonPressed() {
    showSettingsView();
}

void FTPServer::onSettingsButtonCallback(lv_event_t* event) {
    auto* app = static_cast<FTPServer*>(lv_event_get_user_data(event));
    app->onSettingsButtonPressed();
}

void FTPServer::onClearLogButtonPressed() {
    mainView.clearLog();
}

void FTPServer::onClearLogButtonCallback(lv_event_t* event) {
    auto* app = static_cast<FTPServer*>(lv_event_get_user_data(event));
    app->onClearLogButtonPressed();
}

// Timer callback to check FTP server status after start
static void ftpStartCheckTimerCallback(lv_timer_t* timer) {
    auto* app = static_cast<FTPServer*>(lv_timer_get_user_data(timer));
    if (app != nullptr) {
        app->checkFtpServerStarted();
    }
    lv_timer_delete(timer);
}

void FTPServer::checkFtpServerStarted() {
    // Clear the timer pointer since we're being called (timer will be deleted after this)
    ftpStartCheckTimer = nullptr;

    if (activeView != &mainView) {
        return;
    }

    if (ftpServer && ftpServer->isEnabled()) {
        mainView.updateInfoPanel(nullptr, "Running", LV_PALETTE_GREEN);
        mainView.logToScreen("FTP Server started!");

        char userpassStr[100];
        snprintf(userpassStr, sizeof(userpassStr), "User: %s  Pass: %s  Port: %d", ftpUsername, ftpPassword, ftpPort);
        mainView.logToScreen(userpassStr);
        mainView.logToScreen("Ready for connections...");
        if (settingsButton != nullptr) {
            lv_obj_add_state(settingsButton, LV_STATE_DISABLED);
            lv_obj_add_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        if (spinner != nullptr) {
            lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        }
        mainView.updateInfoPanel(nullptr, "Error", LV_PALETTE_RED);
        mainView.logToScreen("ERROR: Failed to start FTP server!");
        if (connectSwitch != nullptr) {
            lv_obj_remove_state(connectSwitch, LV_STATE_CHECKED);
        }
    }
}

void FTPServer::onSwitchToggled(bool checked) {
    ESP_LOGI(TAG, "Switch toggled: %d", checked);

    if (checked) {
        if (!isWifiConnected()) {
            ESP_LOGI(TAG, "WiFi not connected");
            mainView.showWifiPrompt();
            lv_obj_remove_state(connectSwitch, LV_STATE_CHECKED);
            return;
        }

        // Get IP for display
        char ipStr[32] = "IP: --";
        esp_netif_ip_info_t ipInfo;
        if (esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ipInfo) == ESP_OK) {
            snprintf(ipStr, sizeof(ipStr), "IP: " IPSTR, IP2STR(&ipInfo.ip));
        }

        mainView.updateInfoPanel(ipStr, "Starting...", LV_PALETTE_GREEN);
        if (spinner) {
            lv_obj_remove_flag(spinner, LV_OBJ_FLAG_HIDDEN);
        }

        if (ftpServer) {
            ftpServer->setCredentials(ftpUsername, ftpPassword);
            ftpServer->start();

            // Cancel any existing timer before creating a new one
            if (ftpStartCheckTimer != nullptr) {
                lv_timer_delete(ftpStartCheckTimer);
                ftpStartCheckTimer = nullptr;
            }

            // Schedule a timer to check server status after 200ms (non-blocking)
            ftpStartCheckTimer = lv_timer_create(ftpStartCheckTimerCallback, 200, this);
        }
    } else {
        // Cancel any pending start check timer
        if (ftpStartCheckTimer != nullptr) {
            lv_timer_delete(ftpStartCheckTimer);
            ftpStartCheckTimer = nullptr;
        }

        if (ftpServer) {
            ESP_LOGI(TAG, "Stopping FTP Server...");
            ftpServer->stop();
            if (spinner) {
                lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);
            }
            mainView.updateInfoPanel(nullptr, "Stopped", LV_PALETTE_GREY);
            mainView.logToScreen("FTP Server stopped");
            lv_obj_remove_state(settingsButton, LV_STATE_DISABLED);
            lv_obj_remove_flag(settingsButton, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void FTPServer::onSwitchToggledCallback(lv_event_t* event) {
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_VALUE_CHANGED) {
        auto* app = static_cast<FTPServer*>(lv_event_get_user_data(event));
        lv_obj_t* sw = lv_event_get_target_obj(event);
        bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
        app->onSwitchToggled(checked);
    }
}

//==============================================================================================
// App Lifecycle
//==============================================================================================

void FTPServer::onShow(AppHandle appHandle, lv_obj_t* parent) {
    ESP_LOGI(TAG, "onShow called");
    currentAppHandle = appHandle;
    currentInstance = this;

    // Load settings
    if (!loadSettings()) {
        saveSettings();
    }

    // Create FTP server instance
    ftpServer = new FtpServer::Server();
    if (!ftpServer) {
        ESP_LOGE(TAG, "Failed to create FTP server!");
        currentAppHandle = nullptr;
        currentInstance = nullptr;
        return;
    }

    ftpServer->setCredentials(ftpUsername, ftpPassword);
    ftpServer->setPort(static_cast<uint16_t>(ftpPort));

    ftpServer->register_screen_log_callback([](const char* msg) {
        if (currentInstance && currentInstance->mainView.hasValidLogArea()) {
            currentInstance->mainView.logToScreen(msg);
        }
    });

    // Setup parent layout
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    // Toolbar
    toolbar = tt_lvgl_toolbar_create_for_app(parent, appHandle);

    // Add spinner to toolbar (hidden initially)
    spinner = tt_lvgl_toolbar_add_spinner_action(toolbar);
    lv_obj_add_flag(spinner, LV_OBJ_FLAG_HIDDEN);

    // Add settings button to toolbar
    settingsButton = tt_lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_SETTINGS, onSettingsButtonCallback, this);

    // Add clear log button to toolbar
    clearLogButton = tt_lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_TRASH, onClearLogButtonCallback, this);

    // Add switch to toolbar
    connectSwitch = tt_lvgl_toolbar_add_switch_action(toolbar);
    lv_obj_add_event_cb(connectSwitch, onSwitchToggledCallback, LV_EVENT_VALUE_CHANGED, this);

    // Create wrapper widget for view swapping
    wrapperWidget = lv_obj_create(parent);
    lv_obj_set_width(wrapperWidget, LV_PCT(100));
    lv_obj_set_flex_grow(wrapperWidget, 1);
    lv_obj_set_style_pad_all(wrapperWidget, 0, 0);
    lv_obj_set_style_border_width(wrapperWidget, 0, 0);
    lv_obj_set_style_bg_opa(wrapperWidget, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(wrapperWidget, LV_OBJ_FLAG_SCROLLABLE);

    // Show main view
    showMainView();

    ESP_LOGI(TAG, "UI created successfully");
}

void FTPServer::onHide(AppHandle context) {
    ESP_LOGI(TAG, "onHide called");

    // Cancel pending timer to prevent callback after teardown
    if (ftpStartCheckTimer != nullptr) {
        lv_timer_delete(ftpStartCheckTimer);
        ftpStartCheckTimer = nullptr;
    }

    // Stop active view
    stopActiveView();

    // Save settings
    saveSettings();

    // Stop FTP server
    if (ftpServer) {
        ftpServer->stop();
        delete ftpServer;
        ftpServer = nullptr;
    }

    // Clear handles
    currentAppHandle = nullptr;
    currentInstance = nullptr;
    wrapperWidget = nullptr;
    toolbar = nullptr;
    settingsButton = nullptr;
    spinner = nullptr;
    connectSwitch = nullptr;
    clearLogButton = nullptr;
}
