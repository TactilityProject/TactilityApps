// SPDX-License-Identifier: GPL-3.0-only
#include "Util.h"

#include <tactility/log.h>

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

constexpr auto* TAG = "WebServerUtil";

namespace util {

std::string format(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    const int length = vsnprintf(nullptr, 0, format, arguments);
    va_end(arguments);

    std::string result;
    if (length > 0) {
        result.resize(static_cast<size_t>(length));
        vsnprintf(result.data(), result.size() + 1, format, copy);
    }
    va_end(copy);
    return result;
}

std::string escapeJson(const std::string& text) {
    std::string result;
    result.reserve(text.size());
    for (char c : text) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    result += format("\\u%04x", static_cast<int>(c));
                } else {
                    result += c;
                }
        }
    }
    return result;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::string percentDecode(const std::string& text) {
    std::string decoded;
    decoded.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 < text.size()) {
                int high = hexValue(text[i + 1]);
                int low = hexValue(text[i + 2]);
                if (high >= 0 && low >= 0) {
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    i += 2;
                    continue;
                }
            }
            // A malformed escape is kept as it is
            decoded.push_back(c);
        } else if (c == '+') {
            decoded.push_back(' ');
        } else if (static_cast<unsigned char>(c) > 31) {
            decoded.push_back(c);
        }
    }
    return decoded;
}

std::string normalizePath(const std::string& raw) {
    std::string text = raw;
    if (text.size() >= 2 && ((text.front() == '\'' && text.back() == '\'') || (text.front() == '"' && text.back() == '"'))) {
        text = text.substr(1, text.size() - 2);
    }

    const std::string decoded = percentDecode(text);

    size_t start = 0;
    while (start < decoded.size() && isspace(static_cast<unsigned char>(decoded[start]))) ++start;
    size_t end = decoded.size();
    while (end > start && isspace(static_cast<unsigned char>(decoded[end - 1]))) --end;
    std::string trimmed = decoded.substr(start, end - start);

    if (trimmed.empty()) trimmed = "/";
    if (trimmed.front() != '/') trimmed = '/' + trimmed;

    std::string result;
    result.reserve(trimmed.size());
    bool lastWasSlash = false;
    for (char c : trimmed) {
        if (c == '/') {
            if (!lastWasSlash) result.push_back(c);
            lastWasSlash = true;
        } else {
            result.push_back(c);
            lastWasSlash = false;
        }
    }
    return result;
}

bool isAllowedBasePath(const std::string& path, bool allowRoot) {
    if (path.empty()) return false;
    if (path == ".." || path.starts_with("../") || path.find("/../") != std::string::npos || path.ends_with("/..")) {
        return false;
    }
    if (allowRoot && path == "/") return true;
    // The root itself or a path inside it: "/database" or "/sdcard2" must not match
    auto isUnder = [&path](const std::string& root) {
        return path == root || path.starts_with(root + "/");
    };
    return isUnder("/data") || isUnder("/sdcard");
}

const char* getContentType(const std::string& path) {
    auto endsWith = [&path](const char* extension) {
        const size_t length = strlen(extension);
        return path.length() >= length && path.compare(path.length() - length, length, extension) == 0;
    };

    if (endsWith(".html") || endsWith(".htm")) return "text/html";
    if (endsWith(".css")) return "text/css";
    if (endsWith(".js")) return "application/javascript";
    if (endsWith(".json")) return "application/json";
    if (endsWith(".xml")) return "application/xml";
    if (endsWith(".txt")) return "text/plain";

    if (endsWith(".png")) return "image/png";
    if (endsWith(".jpg") || endsWith(".jpeg")) return "image/jpeg";
    if (endsWith(".gif")) return "image/gif";
    if (endsWith(".svg")) return "image/svg+xml";
    if (endsWith(".ico")) return "image/x-icon";
    if (endsWith(".webp")) return "image/webp";

    if (endsWith(".woff")) return "font/woff";
    if (endsWith(".woff2")) return "font/woff2";
    if (endsWith(".ttf")) return "font/ttf";
    if (endsWith(".otf")) return "font/otf";
    if (endsWith(".eot")) return "application/vnd.ms-fontobject";

    if (endsWith(".mp3")) return "audio/mpeg";
    if (endsWith(".wav")) return "audio/wav";
    if (endsWith(".ogg")) return "audio/ogg";
    if (endsWith(".mp4")) return "video/mp4";
    if (endsWith(".webm")) return "video/webm";

    if (endsWith(".pdf")) return "application/pdf";
    if (endsWith(".zip")) return "application/zip";
    if (endsWith(".gz")) return "application/gzip";

    return "application/octet-stream";
}

bool secureCompare(const std::string& a, const std::string& b) {
    const size_t maxLength = std::max(a.size(), b.size());
    volatile unsigned char result = 0;
    result |= (a.size() != b.size());
    for (size_t i = 0; i < maxLength; ++i) {
        unsigned char ca = (i < a.size()) ? static_cast<unsigned char>(a[i]) : 0;
        unsigned char cb = (i < b.size()) ? static_cast<unsigned char>(b[i]) : 0;
        result |= ca ^ cb;
    }
    return result == 0;
}

bool base64Decode(const std::string& encoded, std::string& decoded) {
    decoded.clear();
    uint32_t buffer = 0;
    int bits = 0;
    size_t padding = 0;
    for (char c : encoded) {
        int value;
        if (c >= 'A' && c <= 'Z') value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+') value = 62;
        else if (c == '/') value = 63;
        else if (c == '=') { padding++; continue; }
        else return false;

        // Data after padding is invalid
        if (padding > 0) return false;

        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            decoded.push_back(static_cast<char>((buffer >> bits) & 0xFF));
        }
    }
    return padding <= 2;
}

std::vector<std::string> split(const std::string& input, const std::string& delimiter) {
    std::vector<std::string> result;
    if (delimiter.empty()) {
        result.push_back(input);
        return result;
    }
    size_t start = 0;
    while (true) {
        size_t index = input.find(delimiter, start);
        if (index == std::string::npos) {
            result.push_back(input.substr(start));
            return result;
        }
        result.push_back(input.substr(start, index - start));
        start = index + delimiter.size();
    }
}

// region Files

bool isFile(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode);
}

bool isDirectory(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
}

bool findOrCreateDirectory(const std::string& path, int mode) {
    if (path.empty() || isDirectory(path)) {
        return true;
    }
    for (size_t index = path.find('/', 1); index != std::string::npos; index = path.find('/', index + 1)) {
        std::string parent = path.substr(0, index);
        if (!isDirectory(parent) && mkdir(parent.c_str(), mode) != 0 && !isDirectory(parent)) {
            return false;
        }
    }
    return (mkdir(path.c_str(), mode) == 0) || isDirectory(path);
}

bool findOrCreateParentDirectory(const std::string& path, int mode) {
    const size_t index = path.find_last_of('/');
    if (index == std::string::npos || index == 0) {
        return true;
    }
    return findOrCreateDirectory(path.substr(0, index), mode);
}

bool deleteFile(const std::string& path) {
    return unlink(path.c_str()) == 0;
}

bool deleteRecursively(const std::string& path) {
    if (isDirectory(path)) {
        std::vector<DirectoryEntry> entries;
        if (!listDirectory(path, entries)) {
            return false;
        }
        for (const auto& entry : entries) {
            if (!deleteRecursively(getChildPath(path, entry.name))) {
                return false;
            }
        }
        return rmdir(path.c_str()) == 0;
    }
    return deleteFile(path);
}

std::string getLastPathSegment(const std::string& path) {
    const size_t index = path.find_last_of('/');
    return index == std::string::npos ? path : path.substr(index + 1);
}

std::string getChildPath(const std::string& parent, const std::string& child) {
    if (parent.empty() || parent == "/") {
        return "/" + child;
    }
    return parent + "/" + child;
}

bool listDirectory(const std::string& path, std::vector<DirectoryEntry>& entries) {
    DIR* directory = opendir(path.c_str());
    if (directory == nullptr) {
        return false;
    }
    while (dirent* entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        entries.push_back({ entry->d_name, entry->d_type == DT_DIR || entry->d_type == DT_CHR });
    }
    closedir(directory);
    return true;
}

// endregion

// region HTTP requests

bool getQueryParam(HttpServerRequest* request, const char* key, std::string& value) {
    const size_t length = http_server_request_get_query(request, nullptr, 0);
    if (length == 0) {
        return false;
    }
    std::string query(length, '\0');
    http_server_request_get_query(request, query.data(), length + 1);

    size_t position = 0;
    while (position < query.size()) {
        const size_t ampersand = query.find('&', position);
        const size_t pairEnd = ampersand == std::string::npos ? query.size() : ampersand;
        const size_t equals = query.find('=', position);
        if (equals != std::string::npos && equals < pairEnd && query.compare(position, equals - position, key) == 0) {
            value = query.substr(equals + 1, pairEnd - equals - 1);
            return true;
        }
        if (ampersand == std::string::npos) {
            break;
        }
        position = ampersand + 1;
    }
    return false;
}

bool uriMatches(const char* uri, const char* route) {
    const size_t length = strlen(route);
    return strncmp(uri, route, length) == 0 && (uri[length] == '\0' || uri[length] == '?' || uri[length] == '/');
}

error_t sendFileChunked(HttpServerRequest* request, FILE* file) {
    if (http_server_request_send_chunk_start(request) != ERROR_NONE) {
        return ERROR_UNDEFINED;
    }
    char buffer[512];
    size_t read;
    while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        if (http_server_request_send_chunk(request, buffer, read) != ERROR_NONE) {
            return ERROR_UNDEFINED;
        }
    }
    http_server_request_send_chunk_end(request);
    return ERROR_NONE;
}

static bool getHeaderOrSendError(HttpServerRequest* request, const char* name, std::string& value) {
    const size_t length = http_server_request_get_header(request, name, nullptr, 0);
    if (length == 0) {
        http_server_request_send_error(request, 400, "header missing");
        return false;
    }
    value.resize(length);
    http_server_request_get_header(request, name, value.data(), length + 1);
    return true;
}

bool getMultiPartBoundaryOrSendError(HttpServerRequest* request, std::string& boundary) {
    std::string contentType;
    if (!getHeaderOrSendError(request, "Content-Type", contentType)) {
        return false;
    }

    const size_t index = contentType.find("boundary=");
    if (index == std::string::npos) {
        http_server_request_send_error(request, 400, "boundary not found in Content-Type");
        return false;
    }

    boundary = contentType.substr(index + 9);
    boundary = boundary.substr(0, boundary.find(';'));
    while (!boundary.empty() && boundary.back() == ' ') {
        boundary.pop_back();
    }
    if (boundary.size() >= 2 && boundary.front() == '"' && boundary.back() == '"') {
        boundary = boundary.substr(1, boundary.size() - 2);
    }
    return true;
}

// Reads exactly the requested amount of bytes, because http_server_request_receive() may return fewer per call
static bool receiveExact(HttpServerRequest* request, void* buffer, size_t length) {
    size_t total = 0;
    while (total < length) {
        const int read = http_server_request_receive(request, static_cast<char*>(buffer) + total, length - total);
        if (read <= 0) {
            return false;
        }
        total += static_cast<size_t>(read);
    }
    return true;
}

// Bounds the preamble of a multipart part, so a client can't make this buffer grow forever
constexpr size_t MAX_PREAMBLE_LENGTH = 8192;

std::string receiveTextUntil(HttpServerRequest* request, const std::string& terminator) {
    std::string result;
    while (!result.ends_with(terminator)) {
        if (result.length() >= MAX_PREAMBLE_LENGTH) {
            return "";
        }
        char byte;
        if (!receiveExact(request, &byte, 1)) {
            return "";
        }
        result += byte;
    }
    return result;
}

bool readAndDiscardOrSendError(HttpServerRequest* request, const std::string& expected) {
    std::string buffer(expected.length(), '\0');
    if (!receiveExact(request, buffer.data(), expected.length())) {
        http_server_request_send_error(request, 400, "failed to read discardable data");
        return false;
    }
    if (buffer != expected) {
        http_server_request_send_error(request, 400, "discardable data mismatch");
        return false;
    }
    return true;
}

size_t receiveFile(HttpServerRequest* request, size_t length, const std::string& filePath) {
    constexpr size_t BUFFER_SIZE = 512;
    char buffer[BUFFER_SIZE];
    size_t received = 0;

    FILE* file = fopen(filePath.c_str(), "wb");
    if (file == nullptr) {
        LOG_E(TAG, "Failed to open %s for writing", filePath.c_str());
        return 0;
    }

    while (received < length) {
        const size_t chunk = std::min<size_t>(BUFFER_SIZE, length - received);
        const int read = http_server_request_receive(request, buffer, chunk);
        if (read <= 0) {
            LOG_E(TAG, "Receive failed with %zu bytes to go", length - received);
            break;
        }
        if (fwrite(buffer, 1, static_cast<size_t>(read), file) != static_cast<size_t>(read)) {
            LOG_E(TAG, "Failed to write all bytes");
            break;
        }
        received += static_cast<size_t>(read);
    }

    fclose(file);
    return received;
}

std::vector<std::pair<std::string, std::string>> parseContentDisposition(const std::vector<std::string>& headerLines) {
    std::vector<std::pair<std::string, std::string>> result;
    // Not static: the destructor of a static string needs __dso_handle, which an ELF does not have
    const std::string prefix = "Content-Disposition: ";

    auto header = std::find_if(headerLines.begin(), headerLines.end(), [&prefix](const std::string& line) {
        return line.starts_with(prefix);
    });
    if (header == headerLines.end()) {
        return result;
    }

    for (const auto& part : split(header->substr(prefix.size()), "; ")) {
        const size_t equals = part.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        std::string value = part.substr(equals + 1);
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) {
            value.pop_back();
        }
        // The value is quoted
        result.emplace_back(part.substr(0, equals), value.size() > 2 ? value.substr(1, value.size() - 2) : "");
    }
    return result;
}

// endregion

}
