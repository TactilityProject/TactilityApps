#include "CalendarSync.h"

#include <app/paths.h>
#include <esp_http_client.h>
#include <tactility/error.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>

namespace {

constexpr const char* APP_ID = "tactility.calendar";
constexpr const char* CACHE_FILE = "calendar.cache";
constexpr const char* CACHE_TEMP_FILE = "calendar.cache.tmp";
constexpr char CACHE_MAGIC[] = "TACTILITY_CALENDAR_CACHE_V7\n";
constexpr char TIMING_LEGACY_CACHE_MAGIC[] = "TACTILITY_CALENDAR_CACHE_V6\n";
constexpr char PREVIOUS_CACHE_MAGIC[] = "TACTILITY_CALENDAR_CACHE_V5\n";
constexpr char LEGACY_CACHE_MAGIC[] = "TACTILITY_CALENDAR_CACHE_V4\n";
constexpr size_t MAX_RESPONSE_BYTES = 512 * 1024;
constexpr int HTTP_TIMEOUT_MS = 20000;

struct HttpResponse {
    std::string body;
    bool tooLarge = false;
};

bool getAppFilePath(const char* filename, char* path, size_t size) {
    return app_paths_get_user_data_path(APP_ID, filename, path, size) == ERROR_NONE;
}

void copyText(char* target, size_t size, const char* source) {
    if (size == 0) return;
    snprintf(target, size, "%s", source == nullptr ? "" : source);
}

bool isValidWindowTimestamp(const char* value) {
    if (strlen(value) != 16 || value[8] != 'T' || value[15] != 'Z') return false;
    for (size_t i = 0; i < 15; ++i) {
        if (i != 8 && (value[i] < '0' || value[i] > '9')) return false;
    }
    return true;
}

esp_err_t onHttpEvent(esp_http_client_event_t* event) {
    auto* response = static_cast<HttpResponse*>(event->user_data);
    if (event->event_id == HTTP_EVENT_ON_DATA && response != nullptr && event->data_len > 0) {
        const size_t incoming = static_cast<size_t>(event->data_len);
        if (response->body.size() + incoming > MAX_RESPONSE_BYTES) {
            response->tooLarge = true;
            return ESP_FAIL;
        }
        response->body.append(static_cast<const char*>(event->data), incoming);
    }
    return ESP_OK;
}

bool makeFullCalendarReport(char* report, size_t size, char* error, size_t errorSize) {
    const int written = snprintf(report, size,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
        "<d:prop><d:getetag/><c:calendar-data/></d:prop>"
        "<c:filter><c:comp-filter name=\"VCALENDAR\"><c:comp-filter name=\"VEVENT\"/>"
        "</c:comp-filter></c:filter></c:calendar-query>");
    if (written < 0 || static_cast<size_t>(written) >= size) {
        copyText(error, errorSize, "Could not create the full-calendar CalDAV request.");
        return false;
    }
    return true;
}

bool makeUpcomingWindow(char* startText, size_t startSize,
                        char* endText, size_t endSize,
                        char* error, size_t errorSize) {
    const time_t now = time(nullptr);
    if (now < 1735689600) {
        copyText(error, errorSize, "Set the device clock before syncing.");
        return false;
    }
    const time_t end = now + 30 * 24 * 60 * 60;
    tm startUtc {};
    tm endUtc {};
    if (gmtime_r(&now, &startUtc) == nullptr || gmtime_r(&end, &endUtc) == nullptr) {
        copyText(error, errorSize, "Could not read the device clock.");
        return false;
    }

    if (strftime(startText, startSize, "%Y%m%dT%H%M%SZ", &startUtc) == 0 ||
        strftime(endText, endSize, "%Y%m%dT%H%M%SZ", &endUtc) == 0) {
        copyText(error, errorSize, "Could not format the sync date range.");
        return false;
    }
    return true;
}

bool makeUpcomingReport(char* report, size_t size,
                        char* windowStart, size_t windowStartSize,
                        char* windowEnd, size_t windowEndSize,
                        char* error, size_t errorSize) {
    if (!makeUpcomingWindow(windowStart, windowStartSize, windowEnd, windowEndSize,
                            error, errorSize)) {
        return false;
    }

    const int written = snprintf(report, size,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
        "<d:prop><d:getetag/><c:calendar-data><c:expand start=\"%s\" end=\"%s\"/>"
        "</c:calendar-data></d:prop><c:filter><c:comp-filter name=\"VCALENDAR\">"
        "<c:comp-filter name=\"VEVENT\"><c:time-range start=\"%s\" end=\"%s\"/>"
        "</c:comp-filter></c:comp-filter></c:filter></c:calendar-query>",
        windowStart, windowEnd, windowStart, windowEnd);
    if (written < 0 || static_cast<size_t>(written) >= size) {
        copyText(error, errorSize, "Could not create the CalDAV request.");
        return false;
    }
    return true;
}

bool performReport(const CalendarSyncSettings& settings, const char* report,
                   HttpResponse& response, char* message, size_t messageSize) {
    response.body.reserve(4096);
    esp_http_client_config_t config {};
    config.url = settings.calendarUrl;
    config.username = settings.username;
    config.password = settings.password;
    config.auth_type = HTTP_AUTH_TYPE_BASIC;
    config.timeout_ms = HTTP_TIMEOUT_MS;
    config.buffer_size = 2048;
    config.event_handler = onHttpEvent;
    config.user_data = &response;
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
    config.crt_bundle_attach = esp_crt_bundle_attach;
#else
    copyText(message, messageSize, "This firmware has no HTTPS certificate bundle.");
    return false;
#endif

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        copyText(message, messageSize, "Could not create the HTTPS connection.");
        return false;
    }

    esp_err_t result = esp_http_client_set_method(client, HTTP_METHOD_REPORT);
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Depth", "1");
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Content-Type", "application/xml; charset=utf-8");
    if (result == ESP_OK) result = esp_http_client_set_header(client, "Accept", "application/xml");
    if (result == ESP_OK) result = esp_http_client_set_post_field(client, report, static_cast<int>(strlen(report)));
    if (result == ESP_OK) result = esp_http_client_perform(client);
    const int statusCode = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (response.tooLarge) {
        copyText(message, messageSize, "Calendar response exceeds the 512 KB limit.");
        return false;
    }
    if (result != ESP_OK) {
        copyText(message, messageSize, "Sync failed. Check Wi-Fi, URL and HTTPS certificate.");
        return false;
    }
    if (statusCode < 200 || statusCode >= 300) {
        if (statusCode == 401 || statusCode == 403) {
            copyText(message, messageSize, "CalDAV authentication failed or access was denied.");
        } else {
            snprintf(message, messageSize, "CalDAV server returned HTTP %d.", statusCode);
        }
        return false;
    }
    return true;
}

bool syncCalendar(const CalendarSyncSettings& settings, CalendarSyncResult& sync) {
    if (strncmp(settings.calendarUrl, "https://", 8) != 0 ||
        strpbrk(settings.calendarUrl, "\r\n ") != nullptr) {
        copyText(sync.fullError, sizeof(sync.fullError),
                 "Enter a valid HTTPS calendar collection URL in Settings.");
        copyText(sync.upcomingError, sizeof(sync.upcomingError), sync.fullError);
        return false;
    }

    char fullReport[768] {};
    char upcomingReport[768] {};
    HttpResponse response;
    if (makeFullCalendarReport(fullReport, sizeof(fullReport),
                               sync.fullError, sizeof(sync.fullError)) &&
        performReport(settings, fullReport, response,
                      sync.fullError, sizeof(sync.fullError))) {
        sync.fullSucceeded = calendarParseResponse(response.body, sync.events);
        if (!sync.fullSucceeded) {
            copyText(sync.fullError, sizeof(sync.fullError),
                     "The full-calendar response is incomplete or invalid.");
            sync.events.clear();
        }
    }
    std::string().swap(response.body);
    response.tooLarge = false;

    if (makeUpcomingReport(upcomingReport, sizeof(upcomingReport),
                           sync.upcomingWindowStart, sizeof(sync.upcomingWindowStart),
                           sync.upcomingWindowEnd, sizeof(sync.upcomingWindowEnd),
                           sync.upcomingError, sizeof(sync.upcomingError)) &&
        performReport(settings, upcomingReport, response,
                      sync.upcomingError, sizeof(sync.upcomingError))) {
        sync.upcomingSucceeded = calendarParseResponse(response.body, sync.upcomingEvents);
        if (!sync.upcomingSucceeded) {
            copyText(sync.upcomingError, sizeof(sync.upcomingError),
                     "The upcoming-events response is incomplete or invalid.");
            sync.upcomingEvents.clear();
        }
    }
    const auto eventLess = [](const CalendarEvent& left, const CalendarEvent& right) {
        const int startCompare = strcmp(left.start, right.start);
        if (startCompare != 0) return startCompare < 0;
        const int uidCompare = strcmp(left.uid, right.uid);
        return uidCompare == 0 ? strcmp(left.summary, right.summary) < 0 : uidCompare < 0;
    };
    const auto sameEvent = [](const CalendarEvent& left, const CalendarEvent& right) {
        if (left.uid[0] != '\0' && right.uid[0] != '\0') {
            return strcmp(left.uid, right.uid) == 0 &&
                strcmp(left.occurrenceId, right.occurrenceId) == 0 &&
                left.recurrenceOverride == right.recurrenceOverride &&
                left.cancelled == right.cancelled;
        }
        return left.uid[0] == '\0' && right.uid[0] == '\0' &&
            strcmp(left.occurrenceId, right.occurrenceId) == 0 &&
            left.recurrenceOverride == right.recurrenceOverride &&
            left.cancelled == right.cancelled &&
            strcmp(left.summary, right.summary) == 0;
    };
    const auto eventIdentityLess = [](const CalendarEvent& left, const CalendarEvent& right) {
        const int uidCompare = strcmp(left.uid, right.uid);
        if (uidCompare != 0) return uidCompare < 0;
        const int occurrenceCompare = strcmp(left.occurrenceId, right.occurrenceId);
        if (occurrenceCompare != 0) return occurrenceCompare < 0;
        if (left.recurrenceOverride != right.recurrenceOverride) {
            return left.recurrenceOverride < right.recurrenceOverride;
        }
        if (left.cancelled != right.cancelled) return left.cancelled < right.cancelled;
        return left.uid[0] == '\0' && strcmp(left.summary, right.summary) < 0;
    };
    if (sync.fullSucceeded) {
        std::sort(sync.events.begin(), sync.events.end(), eventIdentityLess);
        sync.events.erase(std::unique(sync.events.begin(), sync.events.end(), sameEvent),
                          sync.events.end());
        std::sort(sync.events.begin(), sync.events.end(), eventLess);
    }
    if (sync.upcomingSucceeded) {
        std::sort(sync.upcomingEvents.begin(), sync.upcomingEvents.end(), eventIdentityLess);
        sync.upcomingEvents.erase(
            std::unique(sync.upcomingEvents.begin(), sync.upcomingEvents.end(), sameEvent),
            sync.upcomingEvents.end());
        std::sort(sync.upcomingEvents.begin(), sync.upcomingEvents.end(), eventLess);
    }
    return sync.fullSucceeded || sync.upcomingSucceeded;
}

bool writeCacheField(FILE* file, const char* value) {
    const size_t length = strlen(value);
    return fprintf(file, "%zu\n", length) >= 0 &&
        fwrite(value, 1, length, file) == length &&
        fputc('\n', file) != EOF;
}

bool readCacheField(FILE* file, char* output, size_t outputSize) {
    char lengthLine[16] {};
    if (fgets(lengthLine, sizeof(lengthLine), file) == nullptr) return false;

    errno = 0;
    char* end = nullptr;
    const unsigned long length = strtoul(lengthLine, &end, 10);
    if (errno != 0 || end == lengthLine || (*end != '\n' && *end != '\0') ||
        length >= outputSize) {
        return false;
    }
    if (fread(output, 1, length, file) != length || fgetc(file) != '\n') return false;
    output[length] = '\0';
    return true;
}

bool readCacheWindow(FILE* file, char* windowStart, size_t windowStartSize,
                     char* windowEnd, size_t windowEndSize) {
    if (!readCacheField(file, windowStart, windowStartSize) ||
        !readCacheField(file, windowEnd, windowEndSize)) {
        return false;
    }
    if (windowStart[0] == '\0' && windowEnd[0] == '\0') return true;
    return isValidWindowTimestamp(windowStart) && isValidWindowTimestamp(windowEnd) &&
        strcmp(windowStart, windowEnd) < 0;
}

bool writeCacheSize(FILE* file, size_t value) {
    return fprintf(file, "%zu\n", value) >= 0;
}

bool readCacheSize(FILE* file, size_t& output) {
    char lengthLine[24] {};
    if (fgets(lengthLine, sizeof(lengthLine), file) == nullptr) return false;

    errno = 0;
    char* end = nullptr;
    const unsigned long value = strtoul(lengthLine, &end, 10);
    if (errno != 0 || end == lengthLine || (*end != '\n' && *end != '\0')) return false;
    output = static_cast<size_t>(value);
    return true;
}

bool writeCacheInteger(FILE* file, int32_t value) {
    return fprintf(file, "%ld\n", static_cast<long>(value)) >= 0;
}

bool readCacheInteger(FILE* file, int32_t& output) {
    char line[24] {};
    if (fgets(line, sizeof(line), file) == nullptr) return false;
    errno = 0;
    char* end = nullptr;
    const long value = strtol(line, &end, 10);
    if (errno != 0 || end == line || (*end != '\n' && *end != '\0') ||
        value < INT32_MIN || value > INT32_MAX) {
        return false;
    }
    output = static_cast<int32_t>(value);
    return true;
}

bool writeCacheEvents(FILE* file, const std::vector<CalendarEvent>& events) {
    if (!writeCacheSize(file, events.size())) return false;
    for (const CalendarEvent& event : events) {
        if (event.timeZoneTransitionCount >
            sizeof(event.timeZoneTransitions) / sizeof(event.timeZoneTransitions[0])) {
            return false;
        }
        if (!writeCacheField(file, event.uid) || !writeCacheField(file, event.occurrenceId) ||
            !writeCacheField(file, event.startDate) ||
            !writeCacheField(file, event.endDateExclusive) || !writeCacheField(file, event.start) ||
            !writeCacheField(file, event.summary) ||
            !writeCacheField(file, event.recurrenceRule) ||
            !writeCacheField(file, event.recurrenceLocalId) ||
            !writeCacheField(file, event.recurrenceTimeZone) ||
            !writeCacheSize(file, event.timeZoneTransitionCount)) {
            return false;
        }
        for (uint8_t i = 0; i < event.timeZoneTransitionCount; ++i) {
            const CalendarTimeZoneTransition& transition = event.timeZoneTransitions[i];
            if (!writeCacheField(file, transition.start) ||
                !writeCacheInteger(file, transition.offsetFrom) ||
                !writeCacheInteger(file, transition.offsetTo) ||
                !writeCacheInteger(file, transition.recurrenceMonth) ||
                !writeCacheInteger(file, transition.recurrenceMonthDay) ||
                !writeCacheInteger(file, transition.recurrenceWeekday) ||
                !writeCacheInteger(file, transition.recurrenceOrdinal) ||
                !writeCacheField(file, transition.repeatsYearly ? "1" : "0")) {
                return false;
            }
        }
        if (!writeCacheField(file, event.excludedDates) ||
            !writeCacheField(file, event.recurrenceOverride ? "1" : "0") ||
            !writeCacheField(file, event.cancelled ? "1" : "0") ||
            !writeCacheField(file, event.recurrenceUnsupported ? "1" : "0") ||
            !writeCacheField(file, event.recurrenceThisAndFuture ? "1" : "0") ||
            !writeCacheField(file, event.excludedDatesIncomplete ? "1" : "0") ||
            !writeCacheField(file, event.timeZoneTransitionsIncomplete ? "1" : "0") ||
            !writeCacheField(file, event.timingStart) ||
            !writeCacheField(file, event.timingEnd) ||
            !writeCacheField(file, event.timingDuration) ||
            !writeCacheField(file, event.timingIncomplete ? "1" : "0")) {
            return false;
        }
    }
    return true;
}

bool readCacheEvents(FILE* file, std::vector<CalendarEvent>& events, bool hasTiming) {
    size_t count = 0;
    if (!readCacheSize(file, count)) return false;
    for (size_t i = 0; i < count; ++i) {
        CalendarEvent event {};
        if (!readCacheField(file, event.uid, sizeof(event.uid)) ||
            !readCacheField(file, event.occurrenceId, sizeof(event.occurrenceId)) ||
            !readCacheField(file, event.startDate, sizeof(event.startDate)) ||
            !readCacheField(file, event.endDateExclusive, sizeof(event.endDateExclusive)) ||
            !readCacheField(file, event.start, sizeof(event.start)) ||
            !readCacheField(file, event.summary, sizeof(event.summary)) ||
            !readCacheField(file, event.recurrenceRule, sizeof(event.recurrenceRule)) ||
            !readCacheField(file, event.recurrenceLocalId, sizeof(event.recurrenceLocalId)) ||
            !readCacheField(file, event.recurrenceTimeZone, sizeof(event.recurrenceTimeZone))) {
            return false;
        }
        size_t transitionCount = 0;
        if (!readCacheSize(file, transitionCount) ||
            transitionCount > sizeof(event.timeZoneTransitions) /
                sizeof(event.timeZoneTransitions[0])) {
            return false;
        }
        event.timeZoneTransitionCount = static_cast<uint8_t>(transitionCount);
        for (uint8_t transitionIndex = 0;
             transitionIndex < event.timeZoneTransitionCount; ++transitionIndex) {
            CalendarTimeZoneTransition& transition =
                event.timeZoneTransitions[transitionIndex];
            int32_t recurrenceMonth = 0;
            int32_t recurrenceMonthDay = 0;
            int32_t recurrenceWeekday = 0;
            int32_t recurrenceOrdinal = 0;
            if (!readCacheField(file, transition.start, sizeof(transition.start)) ||
                !readCacheInteger(file, transition.offsetFrom) ||
                !readCacheInteger(file, transition.offsetTo) ||
                !readCacheInteger(file, recurrenceMonth) ||
                !readCacheInteger(file, recurrenceMonthDay) ||
                !readCacheInteger(file, recurrenceWeekday) ||
                !readCacheInteger(file, recurrenceOrdinal) ||
                recurrenceMonth < INT8_MIN || recurrenceMonth > INT8_MAX ||
                recurrenceMonthDay < INT8_MIN || recurrenceMonthDay > INT8_MAX ||
                recurrenceWeekday < INT8_MIN || recurrenceWeekday > INT8_MAX ||
                recurrenceOrdinal < INT8_MIN || recurrenceOrdinal > INT8_MAX) {
                return false;
            }
            transition.recurrenceMonth = static_cast<int8_t>(recurrenceMonth);
            transition.recurrenceMonthDay = static_cast<int8_t>(recurrenceMonthDay);
            transition.recurrenceWeekday = static_cast<int8_t>(recurrenceWeekday);
            transition.recurrenceOrdinal = static_cast<int8_t>(recurrenceOrdinal);
            char repeatFlag[2] {};
            if (!readCacheField(file, repeatFlag, sizeof(repeatFlag)) ||
                (repeatFlag[0] != '0' && repeatFlag[0] != '1')) {
                return false;
            }
            transition.repeatsYearly = repeatFlag[0] == '1';
        }
        if (!readCacheField(file, event.excludedDates, sizeof(event.excludedDates))) return false;
        char flag[2] {};
        if (!readCacheField(file, flag, sizeof(flag)) || (flag[0] != '0' && flag[0] != '1')) return false;
        event.recurrenceOverride = flag[0] == '1';
        if (!readCacheField(file, flag, sizeof(flag)) || (flag[0] != '0' && flag[0] != '1')) return false;
        event.cancelled = flag[0] == '1';
        if (!readCacheField(file, flag, sizeof(flag)) || (flag[0] != '0' && flag[0] != '1')) return false;
        event.recurrenceUnsupported = flag[0] == '1';
        if (!readCacheField(file, flag, sizeof(flag)) || (flag[0] != '0' && flag[0] != '1')) return false;
        event.recurrenceThisAndFuture = flag[0] == '1';
        if (!readCacheField(file, flag, sizeof(flag)) || (flag[0] != '0' && flag[0] != '1')) return false;
        event.excludedDatesIncomplete = flag[0] == '1';
        if (!readCacheField(file, flag, sizeof(flag)) || (flag[0] != '0' && flag[0] != '1')) return false;
        event.timeZoneTransitionsIncomplete = flag[0] == '1';
        if (hasTiming) {
            if (!readCacheField(file, event.timingStart, sizeof(event.timingStart)) ||
                !readCacheField(file, event.timingEnd, sizeof(event.timingEnd)) ||
                !readCacheField(file, event.timingDuration, sizeof(event.timingDuration)) ||
                !readCacheField(file, flag, sizeof(flag)) ||
                (flag[0] != '0' && flag[0] != '1')) return false;
            event.timingIncomplete = flag[0] == '1';
        } else {
            event.timingIncomplete = true;
        }
        events.push_back(event);
    }
    return true;
}

bool saveCalendarCache(const CalendarSyncSettings& settings, const std::vector<CalendarEvent>& events,
                       const std::vector<CalendarEvent>& upcomingEvents,
                       bool fullCalendarAvailable,
                       const char* upcomingWindowStart,
                       const char* upcomingWindowEnd) {
    if (settings.cacheIdentity[0] == '\0') return false;
    char path[256] {};
    char tempPath[256] {};
    if (!getAppFilePath(CACHE_FILE, path, sizeof(path)) ||
        !getAppFilePath(CACHE_TEMP_FILE, tempPath, sizeof(tempPath))) {
        return false;
    }

    FILE* file = fopen(tempPath, "wb");
    if (file == nullptr) return false;

    bool success = fwrite(CACHE_MAGIC, 1, strlen(CACHE_MAGIC), file) == strlen(CACHE_MAGIC) &&
        writeCacheField(file, settings.calendarUrl) &&
        writeCacheField(file, settings.username) &&
        writeCacheField(file, settings.cacheIdentity) &&
        writeCacheField(file, fullCalendarAvailable ? "1" : "0") &&
        writeCacheField(file, upcomingWindowStart) &&
        writeCacheField(file, upcomingWindowEnd) &&
        writeCacheEvents(file, events) &&
        writeCacheEvents(file, upcomingEvents);
    if (fflush(file) != 0) success = false;
    if (fclose(file) != 0) success = false;
    if (!success) {
        remove(tempPath);
        return false;
    }

    if (remove(path) != 0 && errno != ENOENT) {
        remove(tempPath);
        return false;
    }
    if (rename(tempPath, path) != 0) {
        remove(tempPath);
        return false;
    }
    return true;
}

bool loadCalendarCache(const CalendarSyncSettings& settings, std::vector<CalendarEvent>& events,
                       std::vector<CalendarEvent>& upcomingEvents,
                       bool& fullCalendarAvailable,
                       char* upcomingWindowStart, size_t upcomingWindowStartSize,
                       char* upcomingWindowEnd, size_t upcomingWindowEndSize,
                       bool& found, bool& matchesSettings) {
    found = false;
    matchesSettings = false;
    fullCalendarAvailable = false;
    if (upcomingWindowStartSize > 0) upcomingWindowStart[0] = '\0';
    if (upcomingWindowEndSize > 0) upcomingWindowEnd[0] = '\0';
    char path[256] {};
    if (!getAppFilePath(CACHE_FILE, path, sizeof(path))) return false;

    FILE* file = fopen(path, "rb");
    if (file == nullptr) return errno == ENOENT;
    found = true;

    char header[sizeof(CACHE_MAGIC)] {};
    const bool hasHeader = fgets(header, sizeof(header), file) != nullptr;
    const bool currentFormat = hasHeader && strcmp(header, CACHE_MAGIC) == 0;
    const bool timingLegacyFormat = hasHeader && strcmp(header, TIMING_LEGACY_CACHE_MAGIC) == 0;
    const bool validHeader = currentFormat || timingLegacyFormat ||
        (hasHeader && (strcmp(header, PREVIOUS_CACHE_MAGIC) == 0 ||
                       strcmp(header, LEGACY_CACHE_MAGIC) == 0));
    char cachedUrl[sizeof(settings.calendarUrl)] {};
    char cachedUsername[sizeof(settings.username)] {};
    if (!validHeader || !readCacheField(file, cachedUrl, sizeof(cachedUrl)) ||
        !readCacheField(file, cachedUsername, sizeof(cachedUsername))) {
        fclose(file);
        return false;
    }
    if (!currentFormat && !timingLegacyFormat) {
        fclose(file);
        return true;
    }

    char cachedIdentity[sizeof(settings.cacheIdentity)] {};
    if (!readCacheField(file, cachedIdentity, sizeof(cachedIdentity))) {
        fclose(file);
        return false;
    }
    matchesSettings = settings.cacheIdentity[0] != '\0' &&
        strcmp(cachedUrl, settings.calendarUrl) == 0 &&
        strcmp(cachedUsername, settings.username) == 0 &&
        strcmp(cachedIdentity, settings.cacheIdentity) == 0;
    if (!matchesSettings) {
        fclose(file);
        return true;
    }

    char fullCalendarFlag[2] {};
    const bool validData = readCacheField(file, fullCalendarFlag, sizeof(fullCalendarFlag)) &&
        (fullCalendarFlag[0] == '0' || fullCalendarFlag[0] == '1') &&
        readCacheWindow(file, upcomingWindowStart, upcomingWindowStartSize,
                        upcomingWindowEnd, upcomingWindowEndSize) &&
        readCacheEvents(file, events, currentFormat) &&
        readCacheEvents(file, upcomingEvents, currentFormat) && fgetc(file) == EOF && !ferror(file);
    if (validData) fullCalendarAvailable = fullCalendarFlag[0] == '1';
    fclose(file);
    return validData;
}


} // namespace

bool calendarUpcomingWindowCoversNow(const char* windowStart, const char* windowEnd) {
    if (!isValidWindowTimestamp(windowStart) || !isValidWindowTimestamp(windowEnd) ||
        strcmp(windowStart, windowEnd) >= 0) {
        return false;
    }

    char currentStart[17] {};
    char currentEnd[17] {};
    char error[96] {};
    if (!makeUpcomingWindow(currentStart, sizeof(currentStart),
                            currentEnd, sizeof(currentEnd),
                            error, sizeof(error))) {
        return false;
    }
    return strcmp(windowStart, currentStart) <= 0 &&
        strncmp(windowEnd, currentEnd, 8) >= 0;
}

bool calendarSyncCalendar(const CalendarSyncSettings& settings, CalendarSyncResult& sync) {
    return syncCalendar(settings, sync);
}

bool calendarSaveCache(const CalendarSyncSettings& settings,
                       const std::vector<CalendarEvent>& events,
                       const std::vector<CalendarEvent>& upcomingEvents,
                       bool fullCalendarAvailable,
                       const char* upcomingWindowStart,
                       const char* upcomingWindowEnd) {
    return saveCalendarCache(settings, events, upcomingEvents, fullCalendarAvailable,
                             upcomingWindowStart, upcomingWindowEnd);
}

bool calendarLoadCache(const CalendarSyncSettings& settings,
                       std::vector<CalendarEvent>& events,
                       std::vector<CalendarEvent>& upcomingEvents,
                       bool& fullCalendarAvailable,
                       char* upcomingWindowStart, size_t upcomingWindowStartSize,
                       char* upcomingWindowEnd, size_t upcomingWindowEndSize,
                       bool& found, bool& matchesSettings) {
    return loadCalendarCache(settings, events, upcomingEvents, fullCalendarAvailable,
                             upcomingWindowStart, upcomingWindowStartSize,
                             upcomingWindowEnd, upcomingWindowEndSize,
                             found, matchesSettings);
}
