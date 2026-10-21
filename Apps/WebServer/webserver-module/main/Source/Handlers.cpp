// SPDX-License-Identifier: GPL-3.0-only
#include "Handlers.h"

#include "Util.h"
#include "WebServer.h"

#include <app/install.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/start.h>

#include <tactility/device.h>
#include <tactility/drivers/wifi.h>
#include <tactility/filesystem/file_system.h>
#include <tactility/log.h>
#include <tactility/paths.h>

#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sdkconfig.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

constexpr auto* TAG = "WebServerHandlers";

constexpr auto* PACKAGE_ID = "tactility.webserver";
constexpr size_t MAX_UPLOAD_SIZE = 10 * 1024 * 1024;
constexpr size_t MAX_APP_UPLOAD_SIZE = 20 * 1024 * 1024;

namespace {

// region Authentication

error_t sendUnauthorized(HttpServerRequest* request, const char* message) {
    http_server_request_set_header(request, "WWW-Authenticate", "Basic realm=\"Tactility\"");
    http_server_request_send_error(request, 401, message);
    // The response was sent
    return ERROR_NONE;
}

/**
 * Validates HTTP basic authentication.
 * @param[out] authPassed true when the request may continue. When false, a 401 response was sent already.
 */
error_t validateRequestAuth(HttpServerRequest* request, bool& authPassed) {
    authPassed = false;

    const WebServerSettings settings = serverGetSettings();
    if (!settings.auth_enabled) {
        authPassed = true;
        return ERROR_NONE;
    }

    const size_t headerLength = http_server_request_get_header(request, "Authorization", nullptr, 0);
    if (headerLength == 0) {
        return sendUnauthorized(request, "Authorization required");
    }
    std::string header(headerLength, '\0');
    http_server_request_get_header(request, "Authorization", header.data(), headerLength + 1);

    if (header.rfind("Basic ", 0) != 0) {
        LOG_W(TAG, "The authorization header is not Basic");
        return sendUnauthorized(request, "Basic authorization required");
    }

    std::string credentials;
    if (!util::base64Decode(header.substr(6), credentials)) {
        LOG_W(TAG, "Failed to decode the credentials");
        return sendUnauthorized(request, "Invalid credentials format");
    }

    const size_t colon = credentials.find(':');
    if (colon == std::string::npos) {
        LOG_W(TAG, "The credentials have no colon separator");
        return sendUnauthorized(request, "Invalid credentials format");
    }

    const std::string username = credentials.substr(0, colon);
    const std::string password = credentials.substr(colon + 1);
    const bool usernameMatches = util::secureCompare(username, settings.username);
    const bool passwordMatches = util::secureCompare(password, settings.password);
    if (!usernameMatches || !passwordMatches) {
        LOG_W(TAG, "Invalid credentials for user '%s'", username.c_str());
        return sendUnauthorized(request, "Invalid credentials");
    }

    authPassed = true;
    return ERROR_NONE;
}

// @return false when the request was answered already, with @a result as the result of the handler
bool authenticate(HttpServerRequest* request, error_t& result) {
    bool authPassed = false;
    result = validateRequestAuth(request, authPassed);
    return authPassed;
}

// endregion

void sendJson(HttpServerRequest* request, const std::string& json) {
    http_server_request_set_content_type(request, "application/json");
    http_server_request_send_string(request, json.c_str());
}

// region Files

error_t handleFsList(HttpServerRequest* request) {
    std::string path;
    if (!util::getQueryParam(request, "path", path) || path.empty()) path = "/";
    const std::string normalized = util::normalizePath(path);

    // The root lists the mount points
    if (!util::isAllowedBasePath(normalized, true)) {
        LOG_W(TAG, "GET /fs/list: invalid path '%s'", path.c_str());
        sendJson(request, "{\"error\":\"invalid path\"}");
        return ERROR_NONE;
    }

    std::string json = "{\"path\":\"" + util::escapeJson(normalized) + "\",\"entries\":[";
    if (normalized == "/") {
        struct Context {
            std::string* json;
            uint16_t count;
        } context { &json, 0 };
        file_system_for_each(&context, [](auto* fs, void* data) {
            auto* context = static_cast<Context*>(data);
            char path[128];
            if (file_system_is_mounted(fs) && file_system_get_path(fs, path, sizeof(path)) == ERROR_NONE) {
                if (context->count++ > 0) *context->json += ",";
                *context->json += util::format("{\"name\":\"%s\",\"type\":\"dir\",\"size\":0}", util::escapeJson(path).c_str());
            }
            return true;
        });
    } else {
        std::vector<util::DirectoryEntry> entries;
        if (!util::listDirectory(normalized, entries)) {
            sendJson(request, "{\"error\":\"scan failed\"}");
            return ERROR_NONE;
        }
        bool first = true;
        for (const auto& entry : entries) {
            if (!first) json += ",";
            first = false;
            long size = 0;
            if (!entry.isDirectory) {
                struct stat info {};
                if (stat((normalized + "/" + entry.name).c_str(), &info) == 0) {
                    size = static_cast<long>(info.st_size);
                }
            }
            json += util::format("{\"name\":\"%s\",\"type\":\"%s\",\"size\":%ld}", util::escapeJson(entry.name).c_str(), entry.isDirectory ? "dir" : "file", size);
        }
    }
    json += "]}";

    sendJson(request, json);
    return ERROR_NONE;
}

std::string percentEncode(const std::string& text) {
    std::string result;
    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
            result += static_cast<char>(c);
        } else {
            result += util::format("%%%02X", static_cast<int>(c));
        }
    }
    return result;
}

error_t handleFsDownload(HttpServerRequest* request) {
    std::string path;
    if (!util::getQueryParam(request, "path", path) || path.empty()) {
        http_server_request_send_error(request, 400, "path required");
        return ERROR_UNDEFINED;
    }
    const std::string normalized = util::normalizePath(path);
    if (!util::isAllowedBasePath(normalized) || !util::isFile(normalized)) {
        LOG_W(TAG, "GET /fs/download: not found or invalid path '%s'", path.c_str());
        http_server_request_send_error(request, 404, "not found");
        return ERROR_UNDEFINED;
    }

    http_server_request_set_content_type(request, util::getContentType(normalized));
    const std::string name = util::getLastPathSegment(normalized);
    std::string disposition = "attachment; filename=\"" + name + "\"";
    // The RFC 5987 form is for browsers with UTF-8 names
    const std::string encoded = percentEncode(name);
    if (!encoded.empty()) {
        disposition += "; filename*=UTF-8''" + encoded;
    }
    http_server_request_set_header(request, "Content-Disposition", disposition.c_str());

    FILE* file = fopen(normalized.c_str(), "rb");
    if (file == nullptr) {
        http_server_request_send_error(request, 500, "open failed");
        return ERROR_UNDEFINED;
    }
    const error_t result = util::sendFileChunked(request, file);
    fclose(file);
    return result;
}

error_t handleFsUpload(HttpServerRequest* request) {
    std::string path;
    if (!util::getQueryParam(request, "path", path) || path.empty()) {
        http_server_request_send_error(request, 400, "path required");
        return ERROR_UNDEFINED;
    }

    const std::string normalized = util::normalizePath(path);
    const uint64_t contentLength = http_server_request_get_content_length(request);
    LOG_I(TAG, "POST /fs/upload '%s' (%u bytes)", normalized.c_str(), static_cast<unsigned>(contentLength));

    if (!util::isAllowedBasePath(normalized)) {
        LOG_W(TAG, "POST /fs/upload: invalid path '%s'", path.c_str());
        http_server_request_send_error(request, 403, "invalid path");
        return ERROR_UNDEFINED;
    }

    if (contentLength > MAX_UPLOAD_SIZE) {
        http_server_request_send_error(request, 400, "file too large");
        return ERROR_UNDEFINED;
    }

    // Only after the size check, so rejected uploads don't create directories
    if (!util::findOrCreateParentDirectory(normalized, 0755)) {
        http_server_request_send_error(request, 500, "failed to create parent directory");
        return ERROR_UNDEFINED;
    }

    // Receive into a sibling file, so a failed upload doesn't destroy an existing file at the target
    const std::string partPath = normalized + ".part";
    const size_t received = util::receiveFile(request, static_cast<size_t>(contentLength), partPath);
    if (received != contentLength) {
        util::deleteFile(partPath);
        http_server_request_send_error(request, 500, "upload failed");
        return ERROR_UNDEFINED;
    }

    // FAT's rename() fails when the target exists, so remove the old file first
    if (util::isFile(normalized) && !util::deleteFile(normalized)) {
        util::deleteFile(partPath);
        http_server_request_send_error(request, 500, "failed to replace existing file");
        return ERROR_UNDEFINED;
    }
    if (rename(partPath.c_str(), normalized.c_str()) != 0) {
        LOG_W(TAG, "POST /fs/upload: rename failed: %s", strerror(errno));
        util::deleteFile(partPath);
        http_server_request_send_error(request, 500, "upload failed");
        return ERROR_UNDEFINED;
    }

    http_server_request_set_content_type(request, "text/plain");
    const std::string message = util::format("Uploaded %u bytes", static_cast<unsigned>(received));
    http_server_request_send_string(request, message.c_str());
    return ERROR_NONE;
}

error_t handleFsTree(HttpServerRequest* request) {
    std::string json = "{\"mounts\": [";

    struct Context {
        std::string* json;
        uint16_t count;
    } context { &json, 0 };
    file_system_for_each(&context, [](auto* fs, void* data) {
        auto* context = static_cast<Context*>(data);
        char path[128];
        if (!file_system_is_mounted(fs) || file_system_get_path(fs, path, sizeof(path)) != ERROR_NONE) {
            return true;
        }

        if (context->count++ > 0) *context->json += ",";
        const std::string mountPath = path;
        const std::string name = mountPath.starts_with("/") ? mountPath.substr(1) : mountPath;
        *context->json += util::format("{\"name\":\"%s\",\"path\":\"%s\",\"entries\": [", util::escapeJson(name).c_str(), util::escapeJson(mountPath).c_str());

        std::vector<util::DirectoryEntry> entries;
        if (util::listDirectory(mountPath, entries)) {
            bool first = true;
            for (const auto& entry : entries) {
                if (!first) *context->json += ",";
                first = false;
                *context->json += util::format("{\"name\":\"%s\",\"type\":\"%s\"}", util::escapeJson(entry.name).c_str(), entry.isDirectory ? "dir" : "file");
            }
        }
        *context->json += "]}";
        return true;
    });
    json += "]}";

    sendJson(request, json);
    return ERROR_NONE;
}

error_t handleFsMkdir(HttpServerRequest* request) {
    std::string path;
    if (!util::getQueryParam(request, "path", path) || path.empty()) {
        http_server_request_send_error(request, 400, "path required");
        return ERROR_UNDEFINED;
    }
    const std::string normalized = util::normalizePath(path);
    if (!util::isAllowedBasePath(normalized)) {
        http_server_request_send_error(request, 403, "invalid path");
        return ERROR_UNDEFINED;
    }
    if (!util::findOrCreateDirectory(normalized, 0755)) {
        http_server_request_send_error(request, 500, "mkdir failed");
        return ERROR_UNDEFINED;
    }
    http_server_request_send_string(request, "ok");
    return ERROR_NONE;
}

bool isMountPoint(const std::string& path) {
    return path == "/data" || path == "/sdcard";
}

error_t handleFsDelete(HttpServerRequest* request) {
    std::string path;
    if (!util::getQueryParam(request, "path", path) || path.empty()) {
        http_server_request_send_error(request, 400, "path required");
        return ERROR_UNDEFINED;
    }
    const std::string normalized = util::normalizePath(path);
    if (!util::isAllowedBasePath(normalized)) {
        http_server_request_send_error(request, 403, "invalid path");
        return ERROR_UNDEFINED;
    }
    if (isMountPoint(normalized)) {
        http_server_request_send_error(request, 403, "cannot delete mount point");
        return ERROR_UNDEFINED;
    }
    const bool deleted = util::isDirectory(normalized) ? util::deleteRecursively(normalized)
        : util::isFile(normalized) && util::deleteFile(normalized);
    if (!deleted) {
        http_server_request_send_error(request, 500, "delete failed");
        return ERROR_UNDEFINED;
    }
    http_server_request_send_string(request, "ok");
    return ERROR_NONE;
}

std::string trimSpaces(const std::string& text) {
    size_t start = 0;
    while (start < text.size() && isspace(static_cast<unsigned char>(text[start]))) ++start;
    size_t end = text.size();
    while (end > start && isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(start, end - start);
}

error_t handleFsRename(HttpServerRequest* request) {
    std::string path;
    std::string newName;
    if (!util::getQueryParam(request, "path", path) || path.empty()) {
        http_server_request_send_error(request, 400, "path required");
        return ERROR_UNDEFINED;
    }
    if (!util::getQueryParam(request, "newName", newName) || newName.empty()) {
        http_server_request_send_error(request, 400, "newName required");
        return ERROR_UNDEFINED;
    }
    const std::string normalized = util::normalizePath(path);
    if (!util::isAllowedBasePath(normalized)) {
        http_server_request_send_error(request, 403, "invalid path");
        return ERROR_UNDEFINED;
    }

    // The new name is a plain name, not a path
    // Decode before validating, so an encoded %2F can't slip past the checks below
    newName = trimSpaces(util::percentDecode(newName));
    if (newName.empty() || newName.find('/') != std::string::npos || newName.find('\\') != std::string::npos || newName.find("..") != std::string::npos) {
        http_server_request_send_error(request, 400, "invalid newName");
        return ERROR_UNDEFINED;
    }

    std::string parent = "/";
    const size_t position = normalized.find_last_of('/');
    if (position != std::string::npos) {
        parent = (position == 0) ? "/" : normalized.substr(0, position);
    }
    if (!util::isAllowedBasePath(parent)) {
        http_server_request_send_error(request, 403, "invalid target parent");
        return ERROR_UNDEFINED;
    }

    const std::string target = util::getChildPath(parent, newName);
    if (util::isFile(target) || util::isDirectory(target)) {
        http_server_request_send_error(request, 400, "target exists");
        return ERROR_UNDEFINED;
    }

    if (rename(normalized.c_str(), target.c_str()) != 0) {
        const int error = errno;
        LOG_W(TAG, "rename failed: %s", strerror(error));
        const std::string message = std::string("rename failed: ") + strerror(error);
        http_server_request_send_error(request, 500, message.c_str());
        return ERROR_UNDEFINED;
    }
    http_server_request_send_string(request, "ok");
    return ERROR_NONE;
}

// endregion

// region API

std::string getUri(HttpServerRequest* request) {
    char uri[256];
    http_server_request_get_uri(request, uri, sizeof(uri));
    return uri;
}

error_t handleApiSysinfo(HttpServerRequest* request) {
    std::string json = "{";

    json += util::format("\"firmware\":{\"version\":\"unknown\",\"idf_version\":\"%d.%d.%d\"},", ESP_IDF_VERSION_MAJOR, ESP_IDF_VERSION_MINOR, ESP_IDF_VERSION_PATCH);

#ifdef CONFIG_FREERTOS_NUMBER_OF_CORES
    constexpr int cores = CONFIG_FREERTOS_NUMBER_OF_CORES;
#else
    constexpr int cores = 1;
#endif
    json += util::format("\"chip\":{\"model\":\"%s\",\"cores\":%d,\"revision\":0,\"features\":[],\"flash_size\":0},", CONFIG_IDF_TARGET, cores);

    auto appendHeap = [&json](const char* name, uint32_t capabilities, bool last) {
        json += util::format("\"%s\":{\"free\":%u,\"total\":%u,\"min_free\":0,\"largest_block\":%u}%s",
            name,
            static_cast<unsigned>(heap_caps_get_free_size(capabilities)),
            static_cast<unsigned>(heap_caps_get_total_size(capabilities)),
            static_cast<unsigned>(heap_caps_get_largest_free_block(capabilities)),
            last ? "" : ",");
    };
    appendHeap("heap", MALLOC_CAP_INTERNAL, false);
    appendHeap("psram", MALLOC_CAP_SPIRAM, false);

    json += "\"storage\":{";
    struct Context {
        std::string* json;
        uint16_t count;
    } context { &json, 0 };
    file_system_for_each(&context, [](auto* fs, void* data) {
        auto* context = static_cast<Context*>(data);
        char mountPath[128] = "";
        if (file_system_get_path(fs, mountPath, sizeof(mountPath)) != ERROR_NONE) return true;

        if (context->count++ > 0) *context->json += ",";
        // The free and total sizes aren't available to modules
        *context->json += util::format("\"%s\":{\"free\":0,\"total\":0,\"mounted\":%s}", mountPath[0] == '/' ? mountPath + 1 : mountPath, file_system_is_mounted(fs) ? "true" : "false");
        return true;
    });
    json += "},";

    json += util::format("\"uptime\":%d,\"task_count\":%u,\"features_enabled\":{\"screenshot\":false}}",
        static_cast<int>(esp_timer_get_time() / 1000000),
        static_cast<unsigned>(uxTaskGetNumberOfTasks()));

    sendJson(request, json);
    return ERROR_NONE;
}

error_t handleApiApps(HttpServerRequest* request) {
    std::vector<AppManifest> manifests;
    app_manager_for_each_manifest([](const AppManifest* manifest, void* context) {
        static_cast<std::vector<AppManifest>*>(context)->push_back(*manifest);
    }, &manifests);

    std::string json = "{\"apps\":[";
    bool first = true;
    for (const auto& manifest : manifests) {
        if (!first) json += ",";
        first = false;

        const char* category = "user";
        if (manifest.category == APP_CATEGORY_SYSTEM) category = "system";
        else if (manifest.category == APP_CATEGORY_SETTINGS) category = "settings";

        json += util::format("{\"id\":\"%s\",\"name\":\"%s\",\"category\":\"%s\",\"isExternal\":%s,\"hidden\":%s}",
            util::escapeJson(manifest.id).c_str(),
            util::escapeJson(manifest.name).c_str(),
            category,
            manifest.location.type == APP_LOCATION_PATH ? "true" : "false",
            (manifest.flags & APP_MANIFEST_FLAG_HIDDEN) ? "true" : "false");
    }
    json += "]}";

    sendJson(request, json);
    return ERROR_NONE;
}

error_t handleApiAppsRun(HttpServerRequest* request) {
    std::string appId;
    if (!util::getQueryParam(request, "id", appId) || appId.empty()) {
        http_server_request_send_error(request, 400, "id parameter required");
        return ERROR_UNDEFINED;
    }

    AppStartContext context;
    if (app_start_context_from_id(appId.c_str(), &context) != ERROR_NONE) {
        http_server_request_send_error(request, 404, "app not found");
        return ERROR_UNDEFINED;
    }

    // Starts a new instance next to the ones that are running already
    AppInstanceId instanceId = 0;
    app_start_with_context(&context, &instanceId);

    http_server_request_send_string(request, "ok");
    return ERROR_NONE;
}

error_t handleApiAppsUninstall(HttpServerRequest* request) {
    std::string appId;
    if (!util::getQueryParam(request, "id", appId) || appId.empty()) {
        http_server_request_send_error(request, 400, "id parameter required");
        return ERROR_UNDEFINED;
    }

    AppManifest manifest;
    if (app_manager_find_manifest(appId.c_str(), &manifest) != ERROR_NONE) {
        // Nothing to uninstall
        http_server_request_send_string(request, "ok");
        return ERROR_NONE;
    }

    if (manifest.location.type != APP_LOCATION_PATH) {
        http_server_request_send_error(request, 403, "cannot uninstall system apps");
        return ERROR_UNDEFINED;
    }

    if (app_uninstall(appId.c_str()) != ERROR_NONE) {
        http_server_request_send_error(request, 500, "uninstall failed");
        return ERROR_UNDEFINED;
    }
    http_server_request_send_string(request, "ok");
    return ERROR_NONE;
}

error_t handleApiAppsInstall(HttpServerRequest* request) {
    std::string boundary;
    if (!util::getMultiPartBoundaryOrSendError(request, boundary)) {
        return ERROR_UNDEFINED;
    }

    size_t contentLeft = static_cast<size_t>(http_server_request_get_content_length(request));

    // The part headers end with an empty line
    const std::string headersText = util::receiveTextUntil(request, "\r\n\r\n");
    contentLeft -= std::min(contentLeft, headersText.length());

    std::vector<std::string> headers;
    for (auto& line : util::split(headersText, "\r\n")) {
        if (!line.empty()) headers.push_back(line);
    }

    std::string filename;
    for (const auto& [key, value] : util::parseContentDisposition(headers)) {
        if (key == "filename") filename = value;
    }
    if (filename.empty()) {
        http_server_request_send_error(request, 400, "filename parameter missing");
        return ERROR_UNDEFINED;
    }

    const std::string trailer = util::format("\r\n--%s--\r\n", boundary.c_str());
    if (contentLeft <= trailer.length()) {
        http_server_request_send_error(request, 400, "invalid multipart payload");
        return ERROR_UNDEFINED;
    }

    const size_t fileSize = contentLeft - trailer.length();
    if (fileSize == 0 || fileSize > MAX_APP_UPLOAD_SIZE) {
        http_server_request_send_error(request, 400, "file too large");
        return ERROR_UNDEFINED;
    }

    char dataPath[256];
    if (paths_get_data_path(dataPath, sizeof(dataPath)) != ERROR_NONE) {
        http_server_request_send_error(request, 500, "no data path");
        return ERROR_UNDEFINED;
    }
    const std::string temporaryPath = std::string(dataPath) + "/tactility/tmp";
    if (!util::findOrCreateDirectory(temporaryPath, 0777)) {
        http_server_request_send_error(request, 500, "failed to create temp directory");
        return ERROR_UNDEFINED;
    }

    const std::string safeName = util::getLastPathSegment(filename);
    if (safeName.empty() || safeName.find("..") != std::string::npos || safeName.find('/') != std::string::npos || safeName.find('\\') != std::string::npos) {
        http_server_request_send_error(request, 400, "invalid filename");
        return ERROR_UNDEFINED;
    }
    const std::string filePath = temporaryPath + "/" + safeName;

    if (util::receiveFile(request, fileSize, filePath) != fileSize) {
        util::deleteFile(filePath);
        http_server_request_send_error(request, 500, "failed to save file");
        return ERROR_UNDEFINED;
    }

    if (!util::readAndDiscardOrSendError(request, trailer)) {
        util::deleteFile(filePath);
        return ERROR_UNDEFINED;
    }

    const error_t installResult = app_install(filePath.c_str());
    if (!util::deleteFile(filePath)) {
        LOG_W(TAG, "Failed to delete %s", filePath.c_str());
    }
    if (installResult != ERROR_NONE) {
        http_server_request_send_error(request, 500, "installation failed");
        return ERROR_UNDEFINED;
    }

    http_server_request_send_string(request, "ok");
    return ERROR_NONE;
}

const char* radioStateToString(WifiRadioState radioState, WifiStationState stationState) {
    switch (radioState) {
        case WIFI_RADIO_STATE_ON:
            switch (stationState) {
                case WIFI_STATION_STATE_CONNECTION_PENDING: return "connecting";
                case WIFI_STATION_STATE_CONNECTED: return "connected";
                default: return "on";
            }
        case WIFI_RADIO_STATE_ON_PENDING: return "turning_on";
        case WIFI_RADIO_STATE_OFF: return "off";
        case WIFI_RADIO_STATE_OFF_PENDING: return "turning_off";
        default: return "unknown";
    }
}

error_t handleApiWifi(HttpServerRequest* request) {
    WifiRadioState radioState = WIFI_RADIO_STATE_OFF;
    WifiStationState stationState = WIFI_STATION_STATE_DISCONNECTED;
    char ip[16] = {};
    char ssid[33] = {};
    int32_t rssi = 1;
    bool secure = false;

    Device* wifiDevice = nullptr;
    if (device_get_first_by_type(&WIFI_TYPE, &wifiDevice) == ERROR_NONE) {
        wifi_get_radio_state(wifiDevice, &radioState);
        wifi_get_station_state(wifiDevice, &stationState);
        if (stationState != WIFI_STATION_STATE_DISCONNECTED) {
            wifi_station_get_target_ssid(wifiDevice, ssid);
        }
        if (stationState == WIFI_STATION_STATE_CONNECTED) {
            wifi_station_get_ipv4_address(wifiDevice, ip);
            wifi_station_get_rssi(wifiDevice, &rssi);
            // The driver doesn't report the authentication of the active connection, so it comes from the last scan
            WifiApRecord records[16];
            size_t recordCount = std::size(records);
            if (wifi_get_scan_results(wifiDevice, records, &recordCount) == ERROR_NONE) {
                for (size_t i = 0; i < recordCount; ++i) {
                    if (strcmp(records[i].ssid, ssid) == 0) {
                        secure = records[i].authentication_type != WIFI_AUTHENTICATION_TYPE_OPEN;
                        break;
                    }
                }
            }
        }
        device_put(wifiDevice);
    }

    sendJson(request, util::format("{\"state\":\"%s\",\"ip\":\"%s\",\"ssid\":\"%s\",\"rssi\":%d,\"secure\":%s}",
        radioStateToString(radioState, stationState),
        util::escapeJson(ip).c_str(),
        util::escapeJson(ssid).c_str(),
        static_cast<int>(rssi),
        secure ? "true" : "false"));
    return ERROR_NONE;
}

// endregion

error_t sendAssetFile(HttpServerRequest* request, const std::string& path, const char* contentType) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return ERROR_NOT_FOUND;
    }
    http_server_request_set_content_type(request, contentType);
    const error_t result = util::sendFileChunked(request, file);
    fclose(file);
    return result;
}

}

error_t handleRoot(HttpServerRequest* request, void*) {
    http_server_request_set_status(request, 302);
    http_server_request_set_header(request, "Location", "/dashboard.html");
    return http_server_request_send(request, nullptr, 0);
}

error_t handleFileBrowser(HttpServerRequest* request, void*) {
    http_server_request_set_status(request, 302);
    http_server_request_set_header(request, "Location", "/dashboard.html#files");
    return http_server_request_send(request, nullptr, 0);
}

error_t handleFsGet(HttpServerRequest* request, void*) {
    error_t result;
    if (!authenticate(request, result)) return result;

    const std::string uri = getUri(request);
    if (util::uriMatches(uri.c_str(), "/fs/list")) return handleFsList(request);
    if (util::uriMatches(uri.c_str(), "/fs/download")) return handleFsDownload(request);
    if (util::uriMatches(uri.c_str(), "/fs/tree")) return handleFsTree(request);
    http_server_request_send_error(request, 404, "not found");
    return ERROR_UNDEFINED;
}

error_t handleFsPost(HttpServerRequest* request, void*) {
    error_t result;
    if (!authenticate(request, result)) return result;

    const std::string uri = getUri(request);
    if (util::uriMatches(uri.c_str(), "/fs/mkdir")) return handleFsMkdir(request);
    if (util::uriMatches(uri.c_str(), "/fs/delete")) return handleFsDelete(request);
    if (util::uriMatches(uri.c_str(), "/fs/rename")) return handleFsRename(request);
    if (util::uriMatches(uri.c_str(), "/fs/upload")) return handleFsUpload(request);
    http_server_request_send_error(request, 404, "not found");
    return ERROR_UNDEFINED;
}

error_t handleAdminPost(HttpServerRequest* request, void*) {
    error_t result;
    if (!authenticate(request, result)) return result;

    const std::string uri = getUri(request);
    if (util::uriMatches(uri.c_str(), "/admin/reboot")) {
        http_server_request_send_string(request, "Rebooting...");
        // Gives the response time to arrive
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
        return ERROR_NONE;
    }
    http_server_request_send_error(request, 404, "not found");
    return ERROR_UNDEFINED;
}

error_t handleApiGet(HttpServerRequest* request, void*) {
    const std::string uri = getUri(request);

    // Public, for monitoring
    if (util::uriMatches(uri.c_str(), "/api/sysinfo")) return handleApiSysinfo(request);

    error_t result;
    if (!authenticate(request, result)) return result;

    if (util::uriMatches(uri.c_str(), "/api/apps")) return handleApiApps(request);
    if (util::uriMatches(uri.c_str(), "/api/wifi")) return handleApiWifi(request);
    if (util::uriMatches(uri.c_str(), "/api/screenshot")) {
        http_server_request_send_error(request, 501, "screenshot feature not enabled");
        return ERROR_UNDEFINED;
    }
    http_server_request_send_error(request, 404, "not found");
    return ERROR_UNDEFINED;
}

error_t handleApiPost(HttpServerRequest* request, void*) {
    error_t result;
    if (!authenticate(request, result)) return result;

    const std::string uri = getUri(request);
    if (util::uriMatches(uri.c_str(), "/api/apps/run")) return handleApiAppsRun(request);
    if (util::uriMatches(uri.c_str(), "/api/apps/uninstall")) return handleApiAppsUninstall(request);
    http_server_request_send_error(request, 404, "not found");
    return ERROR_UNDEFINED;
}

error_t handleApiPut(HttpServerRequest* request, void*) {
    error_t result;
    if (!authenticate(request, result)) return result;

    const std::string uri = getUri(request);
    if (util::uriMatches(uri.c_str(), "/api/apps/install")) return handleApiAppsInstall(request);
    http_server_request_send_error(request, 404, "not found");
    return ERROR_UNDEFINED;
}

error_t handleAssets(HttpServerRequest* request, void*) {
    error_t result;
    if (!authenticate(request, result)) return result;

    const std::string uri = getUri(request);
    const std::string requestedPath = util::normalizePath(uri);
    if (requestedPath == "/.." || requestedPath.ends_with("/..") || requestedPath.find("/../") != std::string::npos) {
        http_server_request_send_error(request, 400, "invalid path");
        return ERROR_UNDEFINED;
    }

    // The dashboard files come with the package
    const char* assetName = nullptr;
    const char* contentType = nullptr;
    if (requestedPath == "/favicon.ico") {
        assetName = "favicon.png";
        contentType = "image/png";
        http_server_request_set_header(request, "Cache-Control", "public, max-age=86400");
    } else if (requestedPath == "/dashboard.html") {
        assetName = "dashboard.html";
        contentType = "text/html";
    } else if (requestedPath == "/version.json") {
        assetName = "version.json";
        contentType = "application/json";
    }

    if (assetName != nullptr) {
        char installPath[256];
        if (app_get_install_path(PACKAGE_ID, installPath, sizeof(installPath)) == ERROR_NONE) {
            const std::string assetPath = std::string(installPath) + "/assets/" + assetName;
            const error_t assetResult = sendAssetFile(request, assetPath, contentType);
            if (assetResult != ERROR_NOT_FOUND) {
                return assetResult;
            }
        }
    }

    // Anything else comes from the SD card
    const std::string sdPath = "/sdcard/tactility/webserver" + requestedPath;
    if (util::isFile(sdPath)) {
        const error_t sdResult = sendAssetFile(request, sdPath, util::getContentType(sdPath));
        if (sdResult != ERROR_NOT_FOUND) {
            return sdResult;
        }
    }

    LOG_W(TAG, "[404] %s", uri.c_str());
    http_server_request_send_error(request, 404, "File not found");
    return ERROR_UNDEFINED;
}
