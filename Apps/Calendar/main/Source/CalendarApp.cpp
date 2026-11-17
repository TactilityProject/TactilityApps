#include "CalendarApp.h"

#include <app/event.h>
#include <app/paths.h>
#include <esp_http_client.h>
#if CONFIG_MBEDTLS_CERTIFICATE_BUNDLE
#include <esp_crt_bundle.h>
#endif
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/shared.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/error.h>
#include <tactility/preferences.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

namespace {

constexpr const char* APP_ID = "tactility.calendar";
constexpr const char* SETTINGS_FILE = "settings.properties";
constexpr uint32_t REQUEST_SAVE = 1U;
constexpr uint32_t REQUEST_SYNC = 2U;
constexpr size_t MAX_RESPONSE_BYTES = 48 * 1024;
constexpr int HTTP_TIMEOUT_MS = 20000;

struct HttpResponse {
    std::string body;
    bool tooLarge = false;
};

bool getSettingsPath(char* path, size_t size) {
    return app_paths_get_user_data_path(APP_ID, SETTINGS_FILE, path, size) == ERROR_NONE;
}

void copyText(char* target, size_t size, const char* source) {
    if (size == 0) return;
    snprintf(target, size, "%s", source == nullptr ? "" : source);
}

void signalRequest(CalendarContext* context, uint32_t request) {
    context->requests.fetch_or(request);
    task_event_group_signal(context->eventGroup, context->requestBit);
}

void showSettings(CalendarContext* context, bool visible) {
    context->settingsVisible = visible;
    if (context->agendaPanel == nullptr || context->settingsPanel == nullptr) return;
    if (visible) {
        lv_obj_add_flag(context->agendaPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(context->settingsPanel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(context->settingsPanel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(context->agendaPanel, LV_OBJ_FLAG_HIDDEN);
    }
}

void copySettingsFromWidgets(CalendarContext* context) {
    if (context->urlInput != nullptr) {
        copyText(context->calendarUrl, sizeof(context->calendarUrl), lv_textarea_get_text(context->urlInput));
        copyText(context->username, sizeof(context->username), lv_textarea_get_text(context->usernameInput));
        copyText(context->password, sizeof(context->password), lv_textarea_get_text(context->passwordInput));
    }
}

void onClose(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    app_event_emit_close(context->appInstanceId);
}

void onOpenSettings(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    showSettings(context, true);
}

void onCancelSettings(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    lv_textarea_set_text(context->urlInput, context->calendarUrl);
    lv_textarea_set_text(context->usernameInput, context->username);
    lv_textarea_set_text(context->passwordInput, context->password);
    showSettings(context, false);
}

void onSaveSettings(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    copySettingsFromWidgets(context);
    showSettings(context, false);
    signalRequest(context, REQUEST_SAVE);
}

void onSync(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    if (context->settingsVisible) {
        copySettingsFromWidgets(context);
        showSettings(context, false);
        signalRequest(context, REQUEST_SAVE | REQUEST_SYNC);
    } else {
        signalRequest(context, REQUEST_SYNC);
    }
}

void addInput(lv_obj_t* parent, const char* labelText, const char* placeholder, const char* value,
              uint32_t maxLength, bool password, lv_obj_t** output) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, labelText);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_SMALL), 0);

    lv_obj_t* input = lv_textarea_create(parent);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, maxLength);
    lv_textarea_set_placeholder_text(input, placeholder);
    lv_textarea_set_text(input, value);
    lv_textarea_set_password_mode(input, password);
    lv_obj_set_width(input, LV_PCT(100));
    *output = input;
}

void createButton(lv_obj_t* parent, const char* text, lv_event_cb_t callback, CalendarContext* context) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, context);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
}

void refreshEventList(CalendarContext* context) {
    if (context->eventList == nullptr) return;
    lv_obj_clean(context->eventList);

    if (context->eventCount == 0) {
        lv_obj_t* empty = lv_label_create(context->eventList);
        lv_label_set_text(empty, "No upcoming events. Sync to check this calendar.");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        return;
    }

    for (size_t i = 0; i < context->eventCount; ++i) {
        const CalendarEvent& event = context->events[i];
        lv_obj_t* card = lv_obj_create(context->eventList);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(card, 4, 0);
        lv_obj_set_scrollable(card, false);

        lv_obj_t* date = lv_label_create(card);
        lv_label_set_text(date, event.start);
        lv_obj_set_width(date, LV_PCT(100));
        lv_label_set_long_mode(date, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(date, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
        lv_obj_set_style_text_opa(date, LV_OPA_70, 0);

        lv_obj_t* summary = lv_label_create(card);
        lv_label_set_text(summary, event.summary);
        lv_obj_set_width(summary, LV_PCT(100));
        lv_label_set_long_mode(summary, LV_LABEL_LONG_MODE_WRAP);
    }
}

void setStatus(CalendarContext* context, const char* status) {
    lvgl_lock();
    copyText(context->status, sizeof(context->status), status);
    if (context->statusLabel != nullptr) {
        lv_label_set_text(context->statusLabel, context->status);
    }
    lvgl_unlock();
}

void updateAgenda(CalendarContext* context, const std::array<CalendarEvent, CalendarContext::MAX_EVENTS>& events,
                  size_t count) {
    lvgl_lock();
    context->events = events;
    context->eventCount = count;
    refreshEventList(context);
    lvgl_unlock();
}

void appendCodepoint(std::string& output, unsigned long codepoint) {
    if (codepoint <= 0x7f) {
        output.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7ff) {
        output.push_back(static_cast<char>(0xc0 | (codepoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0xffff) {
        output.push_back(static_cast<char>(0xe0 | (codepoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    } else if (codepoint <= 0x10ffff) {
        output.push_back(static_cast<char>(0xf0 | (codepoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f)));
        output.push_back(static_cast<char>(0x80 | (codepoint & 0x3f)));
    }
}

std::string decodeXml(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size();) {
        if (input.compare(i, 9, "<![CDATA[") == 0) {
            const size_t end = input.find("]]>", i + 9);
            if (end == std::string::npos) {
                output.append(input, i, std::string::npos);
                break;
            }
            output.append(input, i + 9, end - i - 9);
            i = end + 3;
            continue;
        }
        if (input[i] != '&') {
            output.push_back(input[i++]);
            continue;
        }
        const size_t end = input.find(';', i + 1);
        if (end == std::string::npos || end - i > 12) {
            output.push_back(input[i++]);
            continue;
        }
        const std::string entity = input.substr(i + 1, end - i - 1);
        if (entity == "amp") output.push_back('&');
        else if (entity == "lt") output.push_back('<');
        else if (entity == "gt") output.push_back('>');
        else if (entity == "quot") output.push_back('"');
        else if (entity == "apos") output.push_back('\'');
        else if (!entity.empty() && entity[0] == '#') {
            char* parsedEnd = nullptr;
            const int base = entity.size() > 2 && (entity[1] == 'x' || entity[1] == 'X') ? 16 : 10;
            const char* digits = entity.c_str() + (base == 16 ? 2 : 1);
            const unsigned long codepoint = strtoul(digits, &parsedEnd, base);
            if (parsedEnd == digits || *parsedEnd != '\0') {
                output.append(input, i, end - i + 1);
            } else {
                appendCodepoint(output, codepoint);
            }
        } else {
            output.append(input, i, end - i + 1);
        }
        i = end + 1;
    }
    return output;
}

bool equalsIgnoreCase(const std::string& left, const char* right) {
    return left.size() == strlen(right) &&
        std::equal(left.begin(), left.end(), right, [](char a, char b) {
            return std::toupper(static_cast<unsigned char>(a)) == std::toupper(static_cast<unsigned char>(b));
        });
}

std::string unescapeIcs(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '\\' && i + 1 < input.size()) {
            const char escaped = input[++i];
            if (escaped == 'n' || escaped == 'N') output.push_back(' ');
            else output.push_back(escaped);
        } else {
            output.push_back(input[i]);
        }
    }
    return output;
}

void formatStart(const std::string& value, const std::string& parameters, char* output, size_t size) {
    if (value.size() >= 8) {
        char formatted[40] {};
        if (value.size() >= 15 && value[8] == 'T') {
            snprintf(formatted, sizeof(formatted), "%c%c%c%c-%c%c-%c%c %c%c:%c%c",
                value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7],
                value[9], value[10], value[11], value[12]);
            if (value.back() == 'Z') {
                strncat(formatted, " UTC", sizeof(formatted) - strlen(formatted) - 1);
            } else {
                const size_t tzid = parameters.find("TZID=");
                if (tzid != std::string::npos) {
                    size_t end = parameters.find(';', tzid);
                    if (end == std::string::npos) end = parameters.size();
                    strncat(formatted, " ", sizeof(formatted) - strlen(formatted) - 1);
                    strncat(formatted, parameters.substr(tzid + 5, end - tzid - 5).c_str(),
                            sizeof(formatted) - strlen(formatted) - 1);
                }
            }
        } else {
            snprintf(formatted, sizeof(formatted), "%c%c%c%c-%c%c-%c%c",
                value[0], value[1], value[2], value[3], value[4], value[5], value[6], value[7]);
            strncat(formatted, " (all day)", sizeof(formatted) - strlen(formatted) - 1);
        }
        copyText(output, size, formatted);
    } else {
        copyText(output, size, value.c_str());
    }
}

void parseVEvents(const std::string& calendarData,
                  std::array<CalendarEvent, CalendarContext::MAX_EVENTS>& events, size_t& count) {
    std::string unfolded;
    unfolded.reserve(calendarData.size());
    for (size_t i = 0; i < calendarData.size(); ++i) {
        if (calendarData[i] == '\r' && i + 2 < calendarData.size() &&
            calendarData[i + 1] == '\n' && (calendarData[i + 2] == ' ' || calendarData[i + 2] == '\t')) {
            i += 2;
        } else if (calendarData[i] == '\n' && i + 1 < calendarData.size() &&
                   (calendarData[i + 1] == ' ' || calendarData[i + 1] == '\t')) {
            ++i;
        } else if (calendarData[i] != '\r') {
            unfolded.push_back(calendarData[i]);
        }
    }

    bool inEvent = false;
    bool cancelled = false;
    std::string start;
    std::string startParameters;
    std::string summary;
    size_t position = 0;
    while (position <= unfolded.size()) {
        size_t end = unfolded.find('\n', position);
        if (end == std::string::npos) end = unfolded.size();
        const std::string line = unfolded.substr(position, end - position);
        position = end + 1;

        if (equalsIgnoreCase(line, "BEGIN:VEVENT")) {
            inEvent = true;
            cancelled = false;
            start.clear();
            startParameters.clear();
            summary.clear();
        } else if (inEvent && equalsIgnoreCase(line, "END:VEVENT")) {
            if (!cancelled && !start.empty()) {
                CalendarEvent event {};
                formatStart(start, startParameters, event.start, sizeof(event.start));
                const std::string displaySummary = summary.empty() ? "Untitled event" : unescapeIcs(summary);
                copyText(event.summary, sizeof(event.summary), displaySummary.c_str());
                size_t insertion = count;
                while (insertion > 0 && strcmp(event.start, events[insertion - 1].start) < 0) {
                    if (insertion < events.size()) {
                        events[insertion] = events[insertion - 1];
                    }
                    --insertion;
                }
                if (count < events.size()) {
                    ++count;
                    events[insertion] = event;
                } else if (insertion < events.size()) {
                    for (size_t i = events.size() - 1; i > insertion; --i) {
                        events[i] = events[i - 1];
                    }
                    events[insertion] = event;
                }
            }
            inEvent = false;
        } else if (inEvent) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const size_t semicolon = line.find(';');
            const std::string property = line.substr(0, std::min(colon, semicolon));
            const std::string parameters = semicolon != std::string::npos && semicolon < colon
                ? line.substr(semicolon, colon - semicolon)
                : "";
            const std::string value = line.substr(colon + 1);
            if (equalsIgnoreCase(property, "DTSTART")) {
                start = value;
                startParameters = parameters;
            } else if (equalsIgnoreCase(property, "SUMMARY")) {
                summary = value;
            } else if (equalsIgnoreCase(property, "STATUS") && equalsIgnoreCase(value, "CANCELLED")) {
                cancelled = true;
            }
        }
        if (end == unfolded.size()) break;
    }
}

bool parseCalendarResponse(const std::string& xml,
                           std::array<CalendarEvent, CalendarContext::MAX_EVENTS>& events, size_t& count) {
    size_t searchPosition = 0;
    bool foundData = false;
    while (true) {
        const size_t namePosition = xml.find("calendar-data", searchPosition);
        if (namePosition == std::string::npos) break;
        const size_t openStart = xml.rfind('<', namePosition);
        const size_t openEnd = xml.find('>', namePosition);
        if (openStart == std::string::npos || openEnd == std::string::npos) return false;
        const size_t tagEnd = xml.find_first_of(" \t\r\n/>", openStart + 1);
        if (tagEnd == std::string::npos || tagEnd <= namePosition) return false;
        const std::string tagName = xml.substr(openStart + 1, tagEnd - openStart - 1);
        const size_t namespaceSeparator = tagName.rfind(':');
        const std::string localName = tagName.substr(
            namespaceSeparator == std::string::npos ? 0 : namespaceSeparator + 1
        );
        if (localName != "calendar-data") {
            searchPosition = namePosition + strlen("calendar-data");
            continue;
        }
        const std::string closeTag = "</" + tagName + ">";
        const size_t closeStart = xml.find(closeTag, openEnd + 1);
        if (closeStart == std::string::npos) return false;
        parseVEvents(decodeXml(xml.substr(openEnd + 1, closeStart - openEnd - 1)), events, count);
        foundData = true;
        searchPosition = closeStart + closeTag.size();
    }
    return foundData || xml.find("multistatus") != std::string::npos;
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

bool makeReport(char* report, size_t size, char* error, size_t errorSize) {
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

    char startText[17] {};
    char endText[17] {};
    if (strftime(startText, sizeof(startText), "%Y%m%dT%H%M%SZ", &startUtc) == 0 ||
        strftime(endText, sizeof(endText), "%Y%m%dT%H%M%SZ", &endUtc) == 0) {
        copyText(error, errorSize, "Could not format the sync date range.");
        return false;
    }

    const int written = snprintf(report, size,
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
        "<d:prop><d:getetag/><c:calendar-data><c:expand start=\"%s\" end=\"%s\"/>"
        "</c:calendar-data></d:prop><c:filter><c:comp-filter name=\"VCALENDAR\">"
        "<c:comp-filter name=\"VEVENT\"><c:time-range start=\"%s\" end=\"%s\"/>"
        "</c:comp-filter></c:comp-filter></c:filter></c:calendar-query>",
        startText, endText, startText, endText);
    if (written < 0 || static_cast<size_t>(written) >= size) {
        copyText(error, errorSize, "Could not create the CalDAV request.");
        return false;
    }
    return true;
}

bool syncCalendar(CalendarContext* context, char* message, size_t messageSize,
                  std::array<CalendarEvent, CalendarContext::MAX_EVENTS>& events, size_t& count) {
    if (strncmp(context->calendarUrl, "https://", 8) != 0 ||
        strpbrk(context->calendarUrl, "\r\n ") != nullptr) {
        copyText(message, messageSize, "Enter a valid HTTPS calendar collection URL in Settings.");
        return false;
    }

    char report[768] {};
    if (!makeReport(report, sizeof(report), message, messageSize)) return false;

    HttpResponse response;
    response.body.reserve(4096);
    esp_http_client_config_t config {};
    config.url = context->calendarUrl;
    config.username = context->username;
    config.password = context->password;
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
        copyText(message, messageSize, "Calendar response is too large (48 KB limit).");
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
    if (!parseCalendarResponse(response.body, events, count)) {
        copyText(message, messageSize, "The server response did not contain CalDAV calendar data.");
        return false;
    }
    if (count == events.size()) {
        snprintf(message, messageSize, "Showing the first %u upcoming events; more may be available.",
            static_cast<unsigned>(count));
    } else {
        snprintf(message, messageSize, "Synced %u upcoming event%s.",
            static_cast<unsigned>(count), count == 1 ? "" : "s");
    }
    return true;
}

void saveSettings(CalendarContext* context) {
    char path[256];
    if (!getSettingsPath(path, sizeof(path))) {
        setStatus(context, "Could not locate app storage; settings were not saved.");
        return;
    }
    Preferences* preferences = preferences_open(path);
    if (preferences == nullptr) {
        setStatus(context, "Could not open app storage; settings were not saved.");
        return;
    }
    preferences_put_string(preferences, "calendarUrl", context->calendarUrl);
    preferences_put_string(preferences, "username", context->username);
    preferences_put_string(preferences, "password", context->password);
    preferences_close(preferences);
    setStatus(context, "Calendar settings saved.");
}

} // namespace

void calendarLoadSettings(CalendarContext* context) {
    char path[256];
    if (!getSettingsPath(path, sizeof(path))) {
        copyText(context->status, sizeof(context->status), "App storage is unavailable; settings cannot be saved.");
        return;
    }
    Preferences* preferences = preferences_open(path);
    if (preferences == nullptr) {
        copyText(context->status, sizeof(context->status), "Could not read calendar settings.");
        return;
    }
    const error_t urlResult = preferences_opt_string(preferences, "calendarUrl", context->calendarUrl, sizeof(context->calendarUrl));
    const error_t usernameResult = preferences_opt_string(preferences, "username", context->username, sizeof(context->username));
    const error_t passwordResult = preferences_opt_string(preferences, "password", context->password, sizeof(context->password));
    preferences_close(preferences);
    if ((urlResult != ERROR_NONE && urlResult != ERROR_NOT_FOUND) ||
        (usernameResult != ERROR_NONE && usernameResult != ERROR_NOT_FOUND) ||
        (passwordResult != ERROR_NONE && passwordResult != ERROR_NOT_FOUND)) {
        copyText(context->status, sizeof(context->status), "Calendar settings could not be loaded completely.");
        return;
    }
    if (context->calendarUrl[0] != '\0') {
        copyText(context->status, sizeof(context->status), "Ready to sync. Events are shown for the next 30 days.");
    }
}

void calendarCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* context = static_cast<CalendarContext*>(userData);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(parent, false);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Calendar");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onClose, context);

    lv_obj_t* refresh = lvgl_toolbar_add_text_button_action(toolbar, LVGL_ICON_SHARED_REFRESH, onSync, context);
    lv_obj_set_style_text_font(refresh, lvgl_get_shared_icon_default_font(), 0);
    lv_obj_t* settings = lvgl_toolbar_add_text_button_action(toolbar, LVGL_ICON_SHARED_SETTINGS, onOpenSettings, context);
    lv_obj_set_style_text_font(settings, lvgl_get_shared_icon_default_font(), 0);

    context->agendaPanel = lv_obj_create(parent);
    lv_obj_set_width(context->agendaPanel, LV_PCT(100));
    lv_obj_set_flex_grow(context->agendaPanel, 1);
    lv_obj_set_flex_flow(context->agendaPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->agendaPanel, 8, 0);
    lv_obj_set_style_pad_gap(context->agendaPanel, 8, 0);

    context->statusLabel = lv_label_create(context->agendaPanel);
    lv_obj_set_width(context->statusLabel, LV_PCT(100));
    lv_label_set_long_mode(context->statusLabel, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(context->statusLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_label_set_text(context->statusLabel, context->status);

    context->eventList = lv_obj_create(context->agendaPanel);
    lv_obj_set_width(context->eventList, LV_PCT(100));
    lv_obj_set_flex_grow(context->eventList, 1);
    lv_obj_set_flex_flow(context->eventList, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->eventList, 4, 0);
    lv_obj_set_style_pad_gap(context->eventList, 8, 0);
    refreshEventList(context);

    context->settingsPanel = lv_obj_create(parent);
    lv_obj_set_width(context->settingsPanel, LV_PCT(100));
    lv_obj_set_flex_grow(context->settingsPanel, 1);
    lv_obj_set_flex_flow(context->settingsPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->settingsPanel, 8, 0);
    lv_obj_set_style_pad_gap(context->settingsPanel, 6, 0);
    lv_obj_add_flag(context->settingsPanel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* hint = lv_label_create(context->settingsPanel);
    lv_label_set_text(hint, "Use your CalDAV calendar collection URL.");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);

    addInput(context->settingsPanel, "HTTPS calendar URL", "https://server/.../calendar/", context->calendarUrl,
             sizeof(context->calendarUrl) - 1, false, &context->urlInput);
    addInput(context->settingsPanel, "Username", "CalDAV username", context->username,
             sizeof(context->username) - 1, false, &context->usernameInput);
    addInput(context->settingsPanel, "Password", "CalDAV password", context->password,
             sizeof(context->password) - 1, true, &context->passwordInput);

    createButton(context->settingsPanel, "Save", onSaveSettings, context);
    createButton(context->settingsPanel, "Cancel", onCancelSettings, context);
    showSettings(context, context->settingsVisible);
}

void calendarDestroyWidgets(void* userData) {
    auto* context = static_cast<CalendarContext*>(userData);
    context->agendaPanel = nullptr;
    context->settingsPanel = nullptr;
    context->eventList = nullptr;
    context->statusLabel = nullptr;
    context->urlInput = nullptr;
    context->usernameInput = nullptr;
    context->passwordInput = nullptr;
}

void calendarProcessRequests(CalendarContext* context) {
    const uint32_t requests = context->requests.exchange(0);
    if (requests == 0) return;

    if ((requests & REQUEST_SAVE) != 0) {
        saveSettings(context);
    }
    if ((requests & REQUEST_SYNC) != 0) {
        setStatus(context, "Syncing CalDAV calendar...");
        auto events = std::make_unique<std::array<CalendarEvent, CalendarContext::MAX_EVENTS>>();
        size_t count = 0;
        char message[sizeof(context->status)] {};
        if (syncCalendar(context, message, sizeof(message), *events, count)) {
            updateAgenda(context, *events, count);
        }
        setStatus(context, message);
    }
}
