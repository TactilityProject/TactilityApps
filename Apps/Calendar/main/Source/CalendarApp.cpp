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
#include <lvgl/grid_navigation.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/system_bars.h>
#include <tactility/error.h>
#include <tactility/preferences.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {

constexpr const char* APP_ID = "tactility.calendar";
constexpr const char* SETTINGS_FILE = "settings.properties";
constexpr const char* CACHE_FILE = "calendar.cache";
constexpr const char* CACHE_TEMP_FILE = "calendar.cache.tmp";
constexpr char CACHE_MAGIC[] = "TACTILITY_CALENDAR_CACHE_V1\n";
constexpr uint32_t REQUEST_SAVE = 1U;
constexpr uint32_t REQUEST_SYNC = 2U;
constexpr size_t MAX_RESPONSE_BYTES = 512 * 1024;
constexpr int HTTP_TIMEOUT_MS = 20000;

struct HttpResponse {
    std::string body;
    bool tooLarge = false;
};

bool getAppFilePath(const char* filename, char* path, size_t size) {
    return app_paths_get_user_data_path(APP_ID, filename, path, size) == ERROR_NONE;
}

bool getSettingsPath(char* path, size_t size) {
    return getAppFilePath(SETTINGS_FILE, path, size);
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
    if (context->agendaPanel == nullptr || context->monthPanel == nullptr ||
        context->selectedDayPanel == nullptr || context->settingsPanel == nullptr) return;
    if (visible) {
        lv_obj_set_hidden(context->agendaPanel, true);
        lv_obj_set_hidden(context->monthPanel, true);
        lv_obj_set_hidden(context->selectedDayPanel, true);
        lv_obj_set_hidden(context->settingsPanel, false);
    } else {
        lv_obj_set_hidden(context->settingsPanel, true);
        lv_obj_set_hidden(context->agendaPanel, context->page != CalendarPage::Agenda);
        lv_obj_set_hidden(context->monthPanel,
            context->page != CalendarPage::Month || context->dayAgendaVisible);
        lv_obj_set_hidden(context->selectedDayPanel,
            context->page != CalendarPage::Month || !context->dayAgendaVisible);
    }
}

void copySettingsFromWidgets(CalendarContext* context) {
    if (context->urlInput != nullptr) {
        std::lock_guard<std::mutex> lock(context->settingsMutex);
        copyText(context->calendarUrl, sizeof(context->calendarUrl), lv_textarea_get_text(context->urlInput));
        copyText(context->username, sizeof(context->username), lv_textarea_get_text(context->usernameInput));
        copyText(context->password, sizeof(context->password), lv_textarea_get_text(context->passwordInput));
    }
}

struct SettingsSnapshot {
    char calendarUrl[sizeof(CalendarContext::calendarUrl)] {};
    char username[sizeof(CalendarContext::username)] {};
    char password[sizeof(CalendarContext::password)] {};
};

SettingsSnapshot getSettingsSnapshot(CalendarContext* context) {
    std::lock_guard<std::mutex> lock(context->settingsMutex);
    SettingsSnapshot settings {};
    copyText(settings.calendarUrl, sizeof(settings.calendarUrl), context->calendarUrl);
    copyText(settings.username, sizeof(settings.username), context->username);
    copyText(settings.password, sizeof(settings.password), context->password);
    return settings;
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
    const SettingsSnapshot settings = getSettingsSnapshot(context);
    lv_textarea_set_text(context->urlInput, settings.calendarUrl);
    lv_textarea_set_text(context->usernameInput, settings.username);
    lv_textarea_set_text(context->passwordInput, settings.password);
    showSettings(context, false);
}

void onSaveSettings(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    copySettingsFromWidgets(context);
    showSettings(context, false);
    signalRequest(context, REQUEST_SAVE);
}

void refreshEventList(CalendarContext* context);
void refreshMonth(CalendarContext* context);
void refreshMonthAgenda(CalendarContext* context);

void setCalendarPage(CalendarContext* context, CalendarPage page) {
    context->page = page;
    context->dayAgendaVisible = false;
    if (context->agendaPanel == nullptr || context->monthPanel == nullptr ||
        context->selectedDayPanel == nullptr || context->settingsPanel == nullptr) return;
    if (context->settingsVisible) {
        showSettings(context, false);
    }
    lv_obj_set_hidden(context->agendaPanel, page != CalendarPage::Agenda);
    lv_obj_set_hidden(context->monthPanel, page != CalendarPage::Month);
    lv_obj_set_hidden(context->selectedDayPanel, true);
    lv_obj_set_hidden(context->settingsPanel, true);
    if (page == CalendarPage::Month) {
        refreshMonth(context);
    } else {
        refreshEventList(context);
    }
}

void onPreviousPage(void* userData) {
    auto* context = static_cast<CalendarContext*>(userData);
    setCalendarPage(context, CalendarPage::Month);
}

void onNextPage(void* userData) {
    auto* context = static_cast<CalendarContext*>(userData);
    setCalendarPage(context, CalendarPage::Agenda);
}

void requestCalendarSync(CalendarContext* context) {
    if (context->settingsVisible) {
        copySettingsFromWidgets(context);
        showSettings(context, false);
        signalRequest(context, REQUEST_SAVE | REQUEST_SYNC);
    } else {
        signalRequest(context, REQUEST_SYNC);
    }
}

void onSyncMainAction(void* userData) {
    requestCalendarSync(static_cast<CalendarContext*>(userData));
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

void makeArrowButtonCircular(lv_obj_t* button, lv_obj_t* label) {
    lv_obj_update_layout(button);
    const lv_coord_t diameter = std::max(lv_obj_get_width(button), lv_obj_get_height(button));
    lv_obj_set_size(button, diameter, diameter);
    lv_obj_center(label);
}

bool formatDateKey(char* output, size_t size, int year, int month, int day) {
    if (size < 11 || year < 1000 || year > 9999 || month < 1 || month > 12 || day < 1 || day > 31) {
        if (size > 0) output[0] = '\0';
        return false;
    }

    output[0] = static_cast<char>('0' + (year / 1000) % 10);
    output[1] = static_cast<char>('0' + (year / 100) % 10);
    output[2] = static_cast<char>('0' + (year / 10) % 10);
    output[3] = static_cast<char>('0' + year % 10);
    output[4] = '-';
    output[5] = static_cast<char>('0' + (month / 10) % 10);
    output[6] = static_cast<char>('0' + month % 10);
    output[7] = '-';
    output[8] = static_cast<char>('0' + (day / 10) % 10);
    output[9] = static_cast<char>('0' + day % 10);
    output[10] = '\0';
    return true;
}

void refreshEventList(CalendarContext* context) {
    if (context->agendaPanel == nullptr || context->statusLabel == nullptr) return;
    for (uint32_t count = lv_obj_get_child_count(context->agendaPanel); count > 1; --count) {
        lv_obj_delete(lv_obj_get_child(context->agendaPanel, static_cast<int32_t>(count - 1)));
    }

    if (context->upcomingEvents.empty()) {
        lv_obj_t* empty = lv_label_create(context->agendaPanel);
        lv_label_set_text(empty, context->events.empty()
            ? "No cached events. Sync to download the calendar."
            : "No events in the next 30 days.");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        return;
    }

    for (const CalendarEvent& event : context->upcomingEvents) {
        lv_obj_t* card = lv_button_create(context->agendaPanel);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(card, 4, 0);
        lv_obj_set_scrollable(card, false);
        lv_obj_set_scroll_on_focus(card, true);

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

void onDayClicked(lv_event_t* event);

void setDaySelectedStyle(lv_obj_t* day, bool selected) {
    lv_obj_set_style_bg_opa(day, selected ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    if (selected) {
        lv_obj_set_style_bg_color(day, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
    }

    lv_obj_t* number = lv_obj_get_child(day, 0);
    if (number != nullptr) {
        lv_obj_set_style_text_color(number,
            selected ? lv_color_white() : lv_obj_get_style_text_color(day, LV_PART_MAIN),
            LV_PART_MAIN);
    }
    if (lv_obj_get_child_count(day) > 1) {
        lv_obj_t* marker = lv_obj_get_child(day, 1);
        lv_obj_set_style_bg_color(marker, selected
            ? lv_color_white()
            : lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
    }
}

void refreshMonth(CalendarContext* context) {
    if (context->monthTitle == nullptr || context->monthGrid == nullptr) return;

    tm monthStart {};
    monthStart.tm_year = context->displayYear - 1900;
    monthStart.tm_mon = context->displayMonth - 1;
    monthStart.tm_mday = 1;
    monthStart.tm_hour = 12;
    monthStart.tm_isdst = -1;
    mktime(&monthStart);

    const time_t monthStartTime = mktime(&monthStart);
    tm first = *localtime(&monthStartTime);
    const int offset = (first.tm_wday + 6) % 7;
    tm lastDay = monthStart;
    lastDay.tm_mon = context->displayMonth;
    lastDay.tm_mday = 0;
    mktime(&lastDay);
    const int daysInMonth = lastDay.tm_mday;

    char title[32] {};
    strftime(title, sizeof(title), "%B %Y", &monthStart);
    lv_label_set_text(context->monthTitle, title);

    lv_obj_clean(context->monthGrid);
    lv_obj_set_flex_flow(context->monthGrid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(context->monthGrid, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(context->monthGrid, 2, LV_PART_MAIN);

    for (int cell = 0; cell < 42; ++cell) {
        const int dayNumber = cell - offset + 1;
        if (dayNumber < 1 || dayNumber > daysInMonth) {
            lv_obj_t* blank = lv_obj_create(context->monthGrid);
            lv_obj_set_size(blank, LV_PCT(13), LV_PCT(15));
            lv_obj_set_style_border_width(blank, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(blank, LV_OPA_TRANSP, LV_PART_MAIN);
            continue;
        }

        lv_obj_t* day = lv_button_create(context->monthGrid);
        lv_obj_set_size(day, LV_PCT(13), LV_PCT(15));
        lv_obj_set_flex_flow(day, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(day, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(day, 2, LV_PART_MAIN);
        lv_obj_set_style_pad_all(day, 2, LV_PART_MAIN);
        lv_obj_set_style_border_width(day, 0, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(day, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_scroll_on_focus(day, true);
        lv_obj_set_user_data(day, reinterpret_cast<void*>(static_cast<intptr_t>(dayNumber)));
        lv_obj_add_event_cb(day, onDayClicked, LV_EVENT_CLICKED, context);

        const bool selected = context->selectedYear == context->displayYear &&
            context->selectedMonth == context->displayMonth &&
            context->selectedDay == dayNumber;
        setDaySelectedStyle(day, selected);

        lv_obj_t* number = lv_label_create(day);
        char numberText[3] {};
        snprintf(numberText, sizeof(numberText), "%d", dayNumber);
        lv_label_set_text(number, numberText);
        lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        if (selected) {
            lv_obj_set_style_text_color(number, lv_color_white(), LV_PART_MAIN);
        }

        char dateText[11] {};
        formatDateKey(dateText, sizeof(dateText),
            context->displayYear, context->displayMonth, dayNumber);
        bool hasEvent = false;
        for (size_t i = 0; i < context->events.size() && !hasEvent; ++i) {
            hasEvent = strncmp(context->events[i].start, dateText, 10) == 0;
        }
        if (hasEvent) {
            lv_obj_t* marker = lv_obj_create(day);
            lv_obj_set_size(marker, 6, 6);
            lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_border_width(marker, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(marker, selected
                ? lv_color_white()
                : lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
        }
    }
}

void refreshMonthAgenda(CalendarContext* context) {
    if (context->selectedDayTitle == nullptr || context->selectedDayEvents == nullptr) return;

    char dateText[11] {};
    if (!formatDateKey(dateText, sizeof(dateText),
            context->selectedYear, context->selectedMonth, context->selectedDay)) {
        lv_label_set_text(context->selectedDayTitle, "Selected day");
        return;
    }
    lv_label_set_text(context->selectedDayTitle, dateText);

    lv_obj_clean(context->selectedDayEvents);
    size_t shownCount = 0;
    for (size_t i = 0; i < context->events.size(); ++i) {
        if (strncmp(context->events[i].start, dateText, 10) == 0) {
            ++shownCount;
        }
    }
    if (shownCount == 0) {
        lv_obj_t* empty = lv_label_create(context->selectedDayEvents);
        lv_label_set_text(empty, "No events scheduled for this day.");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        return;
    }

    for (size_t i = 0; i < context->events.size(); ++i) {
        const CalendarEvent& event = context->events[i];
        if (strncmp(event.start, dateText, 10) != 0) continue;

        lv_obj_t* card = lv_button_create(context->selectedDayEvents);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(card, 2, LV_PART_MAIN);
        lv_obj_set_scrollable(card, false);

        lv_obj_t* time = lv_label_create(card);
        lv_label_set_text(time, event.start + 11);
        lv_obj_set_width(time, LV_PCT(100));
        lv_obj_set_style_text_font(time, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);

        lv_obj_t* summary = lv_label_create(card);
        lv_label_set_text(summary, event.summary);
        lv_obj_set_width(summary, LV_PCT(100));
        lv_label_set_long_mode(summary, LV_LABEL_LONG_MODE_WRAP);
    }
}

void onDayClicked(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    lv_obj_t* target = lv_event_get_current_target_obj(event);
    const int day = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    context->selectedYear = context->displayYear;
    context->selectedMonth = context->displayMonth;
    context->selectedDay = day;
    context->dayAgendaVisible = true;
    lv_obj_set_hidden(context->monthPanel, true);
    lv_obj_set_hidden(context->selectedDayPanel, false);
    const uint32_t childCount = lv_obj_get_child_count(context->monthGrid);
    for (uint32_t i = 0; i < childCount; ++i) {
        lv_obj_t* dayButton = lv_obj_get_child(context->monthGrid, static_cast<int32_t>(i));
        const int dayNumber = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(dayButton)));
        if (dayNumber > 0) {
            setDaySelectedStyle(dayButton, dayNumber == day);
        }
    }
    refreshMonthAgenda(context);
}

void onBackToMonth(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    context->dayAgendaVisible = false;
    lv_obj_set_hidden(context->selectedDayPanel, true);
    lv_obj_set_hidden(context->monthPanel, false);
}

void changeDisplayedMonth(CalendarContext* context, int offset) {
    tm targetMonth {};
    targetMonth.tm_year = context->displayYear - 1900;
    targetMonth.tm_mon = context->displayMonth - 1 + offset;
    targetMonth.tm_mday = 1;
    targetMonth.tm_hour = 12;
    targetMonth.tm_isdst = -1;
    mktime(&targetMonth);

    const int year = targetMonth.tm_year + 1900;
    if (year < 1000 || year > 9999) return;

    tm lastDay = targetMonth;
    lastDay.tm_mon += 1;
    lastDay.tm_mday = 0;
    mktime(&lastDay);

    context->displayYear = year;
    context->displayMonth = targetMonth.tm_mon + 1;
    context->selectedYear = context->displayYear;
    context->selectedMonth = context->displayMonth;
    context->selectedDay = std::max(1, std::min(context->selectedDay, lastDay.tm_mday));
    refreshMonth(context);
}

void onPreviousMonth(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    changeDisplayedMonth(context, -1);
}

void onNextMonth(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    changeDisplayedMonth(context, 1);
}

void setStatus(CalendarContext* context, const char* status) {
    lvgl_lock();
    copyText(context->status, sizeof(context->status), status);
    if (context->statusLabel != nullptr) {
        lv_label_set_text(context->statusLabel, context->status);
    }
    lvgl_unlock();
}

void updateAgenda(CalendarContext* context, std::vector<CalendarEvent>&& events,
                  std::vector<CalendarEvent>&& upcomingEvents) {
    lvgl_lock();
    context->events = std::move(events);
    context->upcomingEvents = std::move(upcomingEvents);
    refreshEventList(context);
    refreshMonth(context);
    refreshMonthAgenda(context);
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

bool startsWithIgnoreCase(const std::string& value, const char* prefix) {
    const size_t length = strlen(prefix);
    return value.size() >= length &&
        std::equal(value.begin(), value.begin() + length, prefix, [](char a, char b) {
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
                  std::vector<CalendarEvent>& events) {
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
    size_t componentDepth = 0;
    bool cancelled = false;
    std::string uid;
    std::string start;
    std::string startParameters;
    std::string summary;
    size_t position = 0;
    while (position <= unfolded.size()) {
        size_t end = unfolded.find('\n', position);
        if (end == std::string::npos) end = unfolded.size();
        const std::string line = unfolded.substr(position, end - position);
        position = end + 1;

        if (equalsIgnoreCase(line, "BEGIN:VEVENT") && !inEvent) {
            inEvent = true;
            componentDepth = 1;
            cancelled = false;
            uid.clear();
            start.clear();
            startParameters.clear();
            summary.clear();
        } else if (inEvent && startsWithIgnoreCase(line, "BEGIN:")) {
            ++componentDepth;
        } else if (inEvent && equalsIgnoreCase(line, "END:VEVENT") && componentDepth == 1) {
            if (!cancelled && !start.empty()) {
                CalendarEvent event {};
                copyText(event.uid, sizeof(event.uid), uid.c_str());
                formatStart(start, startParameters, event.start, sizeof(event.start));
                const std::string displaySummary = summary.empty() ? "Untitled event" : unescapeIcs(summary);
                copyText(event.summary, sizeof(event.summary), displaySummary.c_str());
                events.push_back(event);
            }
            inEvent = false;
            componentDepth = 0;
        } else if (inEvent && startsWithIgnoreCase(line, "END:")) {
            if (componentDepth > 1) {
                --componentDepth;
            }
        } else if (inEvent && componentDepth == 1) {
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
            } else if (equalsIgnoreCase(property, "UID")) {
                uid = value;
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
                           std::vector<CalendarEvent>& events) {
    size_t position = 0;
    bool foundData = false;
    while ((position = xml.find('<', position)) != std::string::npos) {
        if (xml.compare(position, 4, "<!--") == 0) {
            const size_t commentEnd = xml.find("-->", position + 4);
            if (commentEnd == std::string::npos) return false;
            position = commentEnd + 3;
            continue;
        }
        if (xml.compare(position, 9, "<![CDATA[") == 0) {
            const size_t cdataEnd = xml.find("]]>", position + 9);
            if (cdataEnd == std::string::npos) return false;
            position = cdataEnd + 3;
            continue;
        }
        const char marker = position + 1 < xml.size() ? xml[position + 1] : '\0';
        if (marker == '/' || marker == '!' || marker == '?') {
            const size_t tagEnd = xml.find('>', position + 1);
            if (tagEnd == std::string::npos) return false;
            position = tagEnd + 1;
            continue;
        }

        const size_t nameStart = position + 1;
        const size_t nameEnd = xml.find_first_of(" \t\r\n/>", nameStart);
        if (nameEnd == std::string::npos || nameEnd == nameStart) return false;
        const std::string tagName = xml.substr(nameStart, nameEnd - nameStart);
        const size_t openEnd = xml.find('>', nameEnd);
        if (openEnd == std::string::npos) return false;
        const size_t namespaceSeparator = tagName.rfind(':');
        const std::string localName = tagName.substr(
            namespaceSeparator == std::string::npos ? 0 : namespaceSeparator + 1
        );
        if (localName != "calendar-data") {
            position = openEnd + 1;
            continue;
        }
        foundData = true;
        if (openEnd > nameEnd && xml[openEnd - 1] == '/') {
            position = openEnd + 1;
            continue;
        }

        const std::string closeTag = "</" + tagName + ">";
        const size_t closeStart = xml.find(closeTag, openEnd + 1);
        if (closeStart == std::string::npos) return false;
        parseVEvents(decodeXml(xml.substr(openEnd + 1, closeStart - openEnd - 1)), events);
        position = closeStart + closeTag.size();
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

bool makeUpcomingReport(char* report, size_t size, char* error, size_t errorSize) {
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

bool performReport(const SettingsSnapshot& settings, const char* report,
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

bool syncCalendar(const SettingsSnapshot& settings, char* message, size_t messageSize,
                  std::vector<CalendarEvent>& events,
                  std::vector<CalendarEvent>& upcomingEvents) {
    if (strncmp(settings.calendarUrl, "https://", 8) != 0 ||
        strpbrk(settings.calendarUrl, "\r\n ") != nullptr) {
        copyText(message, messageSize, "Enter a valid HTTPS calendar collection URL in Settings.");
        return false;
    }

    char fullReport[768] {};
    char upcomingReport[768] {};
    if (!makeFullCalendarReport(fullReport, sizeof(fullReport), message, messageSize) ||
        !makeUpcomingReport(upcomingReport, sizeof(upcomingReport), message, messageSize)) {
        return false;
    }

    HttpResponse response;
    if (!performReport(settings, fullReport, response, message, messageSize)) return false;

    std::vector<CalendarEvent> allEvents;
    if (!parseCalendarResponse(response.body, allEvents)) {
        copyText(message, messageSize, "The full-calendar response did not contain CalDAV data.");
        return false;
    }
    std::string().swap(response.body);

    if (!performReport(settings, upcomingReport, response, message, messageSize)) return false;

    std::vector<CalendarEvent> expandedEvents;
    if (!parseCalendarResponse(response.body, expandedEvents)) {
        copyText(message, messageSize, "The upcoming-events response did not contain CalDAV data.");
        return false;
    }
    const auto eventLess = [](const CalendarEvent& left, const CalendarEvent& right) {
        const int startCompare = strcmp(left.start, right.start);
        if (startCompare != 0) return startCompare < 0;
        const int uidCompare = strcmp(left.uid, right.uid);
        return uidCompare == 0 ? strcmp(left.summary, right.summary) < 0 : uidCompare < 0;
    };
    const auto sameEvent = [](const CalendarEvent& left, const CalendarEvent& right) {
        if (strcmp(left.start, right.start) != 0) return false;
        if (left.uid[0] != '\0' && right.uid[0] != '\0') {
            return strcmp(left.uid, right.uid) == 0;
        }
        return left.uid[0] == '\0' && right.uid[0] == '\0' &&
            strcmp(left.summary, right.summary) == 0;
    };
    std::sort(expandedEvents.begin(), expandedEvents.end(), eventLess);
    expandedEvents.erase(std::unique(expandedEvents.begin(), expandedEvents.end(), sameEvent),
                         expandedEvents.end());
    allEvents.insert(allEvents.end(), expandedEvents.begin(), expandedEvents.end());
    std::sort(allEvents.begin(), allEvents.end(), eventLess);
    allEvents.erase(std::unique(allEvents.begin(), allEvents.end(), sameEvent), allEvents.end());

    events = std::move(allEvents);
    upcomingEvents = std::move(expandedEvents);
    snprintf(message, messageSize, "Downloaded %zu calendar events.", events.size());
    return true;
}

void saveSettings(CalendarContext* context) {
    const SettingsSnapshot settings = getSettingsSnapshot(context);
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
    preferences_put_string(preferences, "calendarUrl", settings.calendarUrl);
    preferences_put_string(preferences, "username", settings.username);
    preferences_put_string(preferences, "password", settings.password);
    preferences_close(preferences);
    setStatus(context, "Calendar settings saved.");
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

bool writeCacheEvents(FILE* file, const std::vector<CalendarEvent>& events) {
    if (!writeCacheSize(file, events.size())) return false;
    for (const CalendarEvent& event : events) {
        if (!writeCacheField(file, event.uid) || !writeCacheField(file, event.start) ||
            !writeCacheField(file, event.summary)) {
            return false;
        }
    }
    return true;
}

bool readCacheEvents(FILE* file, std::vector<CalendarEvent>& events) {
    size_t count = 0;
    if (!readCacheSize(file, count)) return false;
    for (size_t i = 0; i < count; ++i) {
        CalendarEvent event {};
        if (!readCacheField(file, event.uid, sizeof(event.uid)) ||
            !readCacheField(file, event.start, sizeof(event.start)) ||
            !readCacheField(file, event.summary, sizeof(event.summary))) {
            return false;
        }
        events.push_back(event);
    }
    return true;
}

bool saveCalendarCache(const SettingsSnapshot& settings, const std::vector<CalendarEvent>& events,
                       const std::vector<CalendarEvent>& upcomingEvents) {
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

bool loadCalendarCache(const SettingsSnapshot& settings, std::vector<CalendarEvent>& events,
                       std::vector<CalendarEvent>& upcomingEvents,
                       bool& found, bool& matchesSettings) {
    found = false;
    matchesSettings = false;
    char path[256] {};
    if (!getAppFilePath(CACHE_FILE, path, sizeof(path))) return false;

    FILE* file = fopen(path, "rb");
    if (file == nullptr) return errno == ENOENT;
    found = true;

    char header[sizeof(CACHE_MAGIC)] {};
    const bool validHeader = fgets(header, sizeof(header), file) != nullptr &&
        strcmp(header, CACHE_MAGIC) == 0;
    char cachedUrl[sizeof(CalendarContext::calendarUrl)] {};
    char cachedUsername[sizeof(CalendarContext::username)] {};
    if (!validHeader || !readCacheField(file, cachedUrl, sizeof(cachedUrl)) ||
        !readCacheField(file, cachedUsername, sizeof(cachedUsername))) {
        fclose(file);
        return false;
    }
    matchesSettings = strcmp(cachedUrl, settings.calendarUrl) == 0 &&
        strcmp(cachedUsername, settings.username) == 0;
    if (!matchesSettings) {
        fclose(file);
        return true;
    }

    const bool validData = readCacheEvents(file, events) &&
        readCacheEvents(file, upcomingEvents) && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return validData;
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
        copyText(context->status, sizeof(context->status), "Ready to sync the full calendar.");
    }
}

void calendarLoadCachedEvents(CalendarContext* context) {
    const SettingsSnapshot settings = getSettingsSnapshot(context);
    if (settings.calendarUrl[0] == '\0') return;

    std::vector<CalendarEvent> events;
    std::vector<CalendarEvent> upcomingEvents;
    bool found = false;
    bool matchesSettings = false;
    if (!loadCalendarCache(settings, events, upcomingEvents, found, matchesSettings)) {
        copyText(context->status, sizeof(context->status), "Saved calendar cache is invalid; sync to refresh.");
        return;
    }
    if (!found || !matchesSettings) return;

    context->events = std::move(events);
    context->upcomingEvents = std::move(upcomingEvents);
    snprintf(context->status, sizeof(context->status),
        "Loaded %zu cached events. Sync to update.", context->events.size());
}

void calendarCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* context = static_cast<CalendarContext*>(userData);
    const SettingsSnapshot settingsSnapshot = getSettingsSnapshot(context);
    if (context->displayYear == 0) {
        const time_t now = time(nullptr);
        tm local {};
        if (localtime_r(&now, &local) != nullptr) {
            context->displayYear = local.tm_year + 1900;
            context->displayMonth = local.tm_mon + 1;
            context->selectedYear = context->displayYear;
            context->selectedMonth = context->displayMonth;
            context->selectedDay = local.tm_mday;
        }
    }
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(parent, false);
    lv_obj_set_style_border_width(parent, 0, LV_PART_MAIN);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Calendar");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onClose, context);

    lv_obj_t* settings = lvgl_toolbar_add_text_button_action(toolbar, LVGL_ICON_SHARED_SETTINGS, onOpenSettings, context);
    lv_obj_set_style_text_font(settings, lvgl_get_shared_icon_default_font(), 0);
    system_bars_set_main_action(LVGL_ICON_SHARED_REFRESH, onSyncMainAction, context);
    system_bars_set_page_button_callbacks(onPreviousPage, onNextPage, context);
    system_bars_set_page_buttons_visible(true);

    context->monthPanel = lv_obj_create(parent);
    lv_obj_set_width(context->monthPanel, LV_PCT(100));
    lv_obj_set_flex_grow(context->monthPanel, 1);
    lv_obj_set_flex_flow(context->monthPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->monthPanel, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(context->monthPanel, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(context->monthPanel, 0, LV_PART_MAIN);

    lv_obj_t* monthNavigation = lv_obj_create(context->monthPanel);
    lv_obj_set_width(monthNavigation, LV_PCT(100));
    lv_obj_set_height(monthNavigation, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(monthNavigation, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(monthNavigation, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(monthNavigation, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(monthNavigation, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(monthNavigation, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(monthNavigation, LV_OPA_TRANSP, LV_PART_MAIN);

    lv_obj_t* previousSlot = lv_obj_create(monthNavigation);
    lv_obj_set_size(previousSlot, LV_PCT(25), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(previousSlot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(previousSlot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(previousSlot, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(previousSlot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(previousSlot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollable(previousSlot, false);

    lv_obj_t* previousMonth = lv_button_create(previousSlot);
    lv_obj_set_size(previousMonth, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(previousMonth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(previousMonth, onPreviousMonth, LV_EVENT_CLICKED, context);
    lv_obj_t* previousLabel = lv_label_create(previousMonth);
    lv_label_set_text(previousLabel, "<");
    makeArrowButtonCircular(previousMonth, previousLabel);

    context->monthTitle = lv_label_create(monthNavigation);
    lv_obj_set_width(context->monthTitle, LV_PCT(50));
    lv_obj_set_style_text_align(context->monthTitle, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_obj_set_style_text_font(context->monthTitle, lvgl_get_text_font(FONT_SIZE_LARGE), LV_PART_MAIN);

    lv_obj_t* nextSlot = lv_obj_create(monthNavigation);
    lv_obj_set_size(nextSlot, LV_PCT(25), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(nextSlot, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nextSlot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(nextSlot, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(nextSlot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(nextSlot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollable(nextSlot, false);

    lv_obj_t* nextMonth = lv_button_create(nextSlot);
    lv_obj_set_size(nextMonth, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(nextMonth, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_add_event_cb(nextMonth, onNextMonth, LV_EVENT_CLICKED, context);
    lv_obj_t* nextLabel = lv_label_create(nextMonth);
    lv_label_set_text(nextLabel, ">");
    makeArrowButtonCircular(nextMonth, nextLabel);

    context->monthGrid = lv_obj_create(context->monthPanel);
    lv_obj_set_width(context->monthGrid, LV_PCT(100));
    lv_obj_set_flex_grow(context->monthGrid, 1);
    lv_obj_set_style_border_width(context->monthGrid, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(context->monthGrid, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(context->monthGrid, LV_SCROLLBAR_MODE_OFF);
    lvgl_grid_navigation_add(context->monthGrid);

    lv_obj_set_hidden(context->monthPanel, true);
    refreshMonth(context);

    context->selectedDayPanel = lv_obj_create(parent);
    lv_obj_set_width(context->selectedDayPanel, LV_PCT(100));
    lv_obj_set_flex_grow(context->selectedDayPanel, 1);
    lv_obj_set_flex_flow(context->selectedDayPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->selectedDayPanel, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(context->selectedDayPanel, 8, LV_PART_MAIN);
    lv_obj_set_style_border_width(context->selectedDayPanel, 0, LV_PART_MAIN);
    createButton(context->selectedDayPanel, "Back to month", onBackToMonth, context);

    context->selectedDayTitle = lv_label_create(context->selectedDayPanel);
    lv_obj_set_width(context->selectedDayTitle, LV_PCT(100));
    lv_obj_set_style_text_font(context->selectedDayTitle, lvgl_get_text_font(FONT_SIZE_LARGE), LV_PART_MAIN);

    context->selectedDayEvents = lv_obj_create(context->selectedDayPanel);
    lv_obj_set_width(context->selectedDayEvents, LV_PCT(100));
    lv_obj_set_flex_grow(context->selectedDayEvents, 1);
    lv_obj_set_flex_flow(context->selectedDayEvents, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->selectedDayEvents, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_gap(context->selectedDayEvents, 4, LV_PART_MAIN);
    lv_obj_set_style_border_width(context->selectedDayEvents, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(context->selectedDayEvents, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(context->selectedDayEvents, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_hidden(context->selectedDayPanel, true);
    refreshMonthAgenda(context);

    context->agendaPanel = lv_obj_create(parent);
    lv_obj_set_width(context->agendaPanel, LV_PCT(100));
    lv_obj_set_flex_grow(context->agendaPanel, 1);
    lv_obj_set_flex_flow(context->agendaPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->agendaPanel, 8, 0);
    lv_obj_set_style_pad_gap(context->agendaPanel, 8, 0);
    lv_obj_set_style_border_width(context->agendaPanel, 0, LV_PART_MAIN);
    lv_obj_set_scroll_dir(context->agendaPanel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(context->agendaPanel, LV_SCROLLBAR_MODE_AUTO);

    context->statusLabel = lv_label_create(context->agendaPanel);
    lv_obj_set_width(context->statusLabel, LV_PCT(100));
    lv_label_set_long_mode(context->statusLabel, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(context->statusLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_label_set_text(context->statusLabel, context->status);

    refreshEventList(context);

    context->settingsPanel = lv_obj_create(parent);
    lv_obj_set_width(context->settingsPanel, LV_PCT(100));
    lv_obj_set_flex_grow(context->settingsPanel, 1);
    lv_obj_set_flex_flow(context->settingsPanel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(context->settingsPanel, 8, 0);
    lv_obj_set_style_pad_gap(context->settingsPanel, 6, 0);
    lv_obj_set_style_border_width(context->settingsPanel, 0, LV_PART_MAIN);
    lv_obj_set_hidden(context->settingsPanel, true);

    lv_obj_t* hint = lv_label_create(context->settingsPanel);
    lv_label_set_text(hint, "Use your CalDAV calendar collection URL.");
    lv_obj_set_width(hint, LV_PCT(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_MODE_WRAP);

    addInput(context->settingsPanel, "HTTPS calendar URL", "https://server/.../calendar/", settingsSnapshot.calendarUrl,
             sizeof(context->calendarUrl) - 1, false, &context->urlInput);
    addInput(context->settingsPanel, "Username", "CalDAV username", settingsSnapshot.username,
             sizeof(context->username) - 1, false, &context->usernameInput);
    addInput(context->settingsPanel, "Password", "CalDAV password", settingsSnapshot.password,
             sizeof(context->password) - 1, true, &context->passwordInput);

    createButton(context->settingsPanel, "Save", onSaveSettings, context);
    createButton(context->settingsPanel, "Cancel", onCancelSettings, context);
    showSettings(context, context->settingsVisible);
    setCalendarPage(context, context->page);
}

void calendarDestroyWidgets(void* userData) {
    auto* context = static_cast<CalendarContext*>(userData);
    system_bars_set_main_action(nullptr, nullptr, nullptr);
    system_bars_set_page_button_callbacks(nullptr, nullptr, nullptr);
    system_bars_set_page_buttons_visible(false);
    context->agendaPanel = nullptr;
    context->monthPanel = nullptr;
    context->monthTitle = nullptr;
    context->monthGrid = nullptr;
    context->selectedDayPanel = nullptr;
    context->selectedDayTitle = nullptr;
    context->selectedDayEvents = nullptr;
    context->settingsPanel = nullptr;
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
        const SettingsSnapshot settings = getSettingsSnapshot(context);
        std::vector<CalendarEvent> events;
        std::vector<CalendarEvent> upcomingEvents;
        char message[sizeof(context->status)] {};
        if (syncCalendar(settings, message, sizeof(message), events, upcomingEvents)) {
            const bool cached = saveCalendarCache(settings, events, upcomingEvents);
            updateAgenda(context, std::move(events), std::move(upcomingEvents));
            if (!cached) {
                copyText(message, sizeof(message),
                    "Calendar downloaded, but its local cache could not be saved.");
            }
        }
        setStatus(context, message);
    }
}
