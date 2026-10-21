// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <http/server.h>

#include <cstddef>
#include <cstdio>
#include <utility>
#include <string>
#include <vector>

namespace util {

/** printf into a std::string */
std::string format(const char* format, ...) __attribute__((format(printf, 1, 2)));

std::string escapeJson(const std::string& text);

/** Decodes the %xx and + sequences and drops control characters */
std::string percentDecode(const std::string& text);

/** Decodes the %xx and + sequences, drops control characters and quotes, makes the path start with one "/" without double slashes */
std::string normalizePath(const std::string& raw);

/** Only paths in /data and /sdcard without ".." components. The root "/" is allowed when @a allowRoot is set. */
bool isAllowedBasePath(const std::string& path, bool allowRoot = false);

const char* getContentType(const std::string& path);

/** Compares in constant time for equal lengths */
bool secureCompare(const std::string& a, const std::string& b);

/** @return false when the text isn't valid base64 */
bool base64Decode(const std::string& encoded, std::string& decoded);

std::vector<std::string> split(const std::string& input, const std::string& delimiter);

// region Files

struct DirectoryEntry {
    std::string name;
    bool isDirectory;
};

bool isFile(const std::string& path);
bool isDirectory(const std::string& path);
/** Creates the directory and its missing parents */
bool findOrCreateDirectory(const std::string& path, int mode);
bool findOrCreateParentDirectory(const std::string& path, int mode);
bool deleteFile(const std::string& path);
bool deleteRecursively(const std::string& path);
std::string getLastPathSegment(const std::string& path);
std::string getChildPath(const std::string& parent, const std::string& child);
/** @return false when the directory can't be read. The entries "." and ".." are left out. */
bool listDirectory(const std::string& path, std::vector<DirectoryEntry>& entries);

// endregion

// region HTTP requests

/** The raw (not decoded) value of a key in the query string */
bool getQueryParam(HttpServerRequest* request, const char* key, std::string& value);

/** @return true when @a uri is @a route, optionally followed by a query or a sub path */
bool uriMatches(const char* uri, const char* route);

/** Sends a streamed file in chunks */
error_t sendFileChunked(HttpServerRequest* request, FILE* file);

bool getMultiPartBoundaryOrSendError(HttpServerRequest* request, std::string& boundary);

/** @return the received text up to and including @a terminator, or "" when the connection failed or the preamble got too long */
std::string receiveTextUntil(HttpServerRequest* request, const std::string& terminator);

bool readAndDiscardOrSendError(HttpServerRequest* request, const std::string& expected);

/** @return the amount of bytes written to the file */
size_t receiveFile(HttpServerRequest* request, size_t length, const std::string& filePath);

/** Reads the name=value pairs of the Content-Disposition header of a multipart part */
std::vector<std::pair<std::string, std::string>> parseContentDisposition(const std::vector<std::string>& headerLines);

// endregion

}
