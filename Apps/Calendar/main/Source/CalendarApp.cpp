#include "CalendarApp.h"

#include <app/event.h>
#include <app/paths.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/shared.h>
#include <lvgl/grid_navigation.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/system_bars.h>
#include <tactility/error.h>
#include <tactility/preferences.h>
#include <esp_random.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <mutex>
#include <utility>

namespace {

constexpr const char* APP_ID = "tactility.calendar";
constexpr const char* SETTINGS_FILE = "settings.properties";
constexpr uint32_t REQUEST_SAVE = 1U;
constexpr uint32_t REQUEST_SYNC = 2U;

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

void createCacheIdentity(char* output, size_t size) {
    snprintf(output, size, "%08x%08x",
             static_cast<unsigned>(esp_random()),
             static_cast<unsigned>(esp_random()));
}

void signalRequest(CalendarContext* context, uint32_t request) {
    context->requests.fetch_or(request);
    task_event_group_signal(context->eventGroup, context->requestBit);
}

void invalidateCalendarData(CalendarContext* context);

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
    if (!visible) {
        lv_obj_t* focus = context->page == CalendarPage::Agenda ? context->agendaPanel
            : context->dayAgendaVisible ? context->selectedDayEvents : context->monthGrid;
        if (focus != nullptr) lv_group_focus_obj(focus);
    }
}

void copySettingsFromWidgets(CalendarContext* context) {
    if (context->urlInput == nullptr) return;
    const char* calendarUrl = lv_textarea_get_text(context->urlInput);
    const char* username = lv_textarea_get_text(context->usernameInput);
    const char* password = lv_textarea_get_text(context->passwordInput);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(context->settingsMutex);
        changed = strcmp(context->calendarUrl, calendarUrl) != 0 ||
            strcmp(context->username, username) != 0 ||
            strcmp(context->password, password) != 0;
        copyText(context->calendarUrl, sizeof(context->calendarUrl), calendarUrl);
        copyText(context->username, sizeof(context->username), username);
        copyText(context->password, sizeof(context->password), password);
        if (changed) {
            createCacheIdentity(context->cacheIdentity, sizeof(context->cacheIdentity));
            context->settingsGeneration.fetch_add(1);
        }
    }
    if (changed) invalidateCalendarData(context);
}

CalendarSyncSettings getCalendarSyncSettings(CalendarContext* context,
                                              uint32_t* generation = nullptr) {
    std::lock_guard<std::mutex> lock(context->settingsMutex);
    CalendarSyncSettings settings {};
    copyText(settings.calendarUrl, sizeof(settings.calendarUrl), context->calendarUrl);
    copyText(settings.username, sizeof(settings.username), context->username);
    copyText(settings.password, sizeof(settings.password), context->password);
    copyText(settings.cacheIdentity, sizeof(settings.cacheIdentity), context->cacheIdentity);
    if (generation != nullptr) *generation = context->settingsGeneration.load();
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
    const CalendarSyncSettings settings = getCalendarSyncSettings(context);
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

void invalidateCalendarData(CalendarContext* context) {
    if (context->statusTimer != nullptr) {
        lv_timer_delete(context->statusTimer);
        context->statusTimer = nullptr;
    }
    context->statusShouldExpire = false;
    context->events.clear();
    context->upcomingEvents.clear();
    context->fullCalendarAvailable = false;
    context->upcomingWindowStart[0] = '\0';
    context->upcomingWindowEnd[0] = '\0';
    copyText(context->status, sizeof(context->status),
             "Calendar changed. Sync to load its events.");
    if (context->statusLabel != nullptr) {
        lv_label_set_text(context->statusLabel, context->status);
        lv_obj_clear_flag(context->statusLabel, LV_OBJ_FLAG_HIDDEN);
    }
    refreshMonth(context);
    refreshMonthAgenda(context);
    refreshEventList(context);
}

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
    lv_group_focus_obj(page == CalendarPage::Month ? context->monthGrid : context->agendaPanel);
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

void refreshEventList(CalendarContext* context) {
    if (context->agendaPanel == nullptr) return;
    for (uint32_t count = lv_obj_get_child_count(context->agendaPanel); count > 0; --count) {
        lv_obj_delete(lv_obj_get_child(context->agendaPanel, static_cast<int32_t>(count - 1)));
    }

    const bool covered =
        context->upcomingEventsGeneration == context->settingsGeneration.load() &&
        calendarUpcomingWindowCoversNow(context->upcomingWindowStart,
                                        context->upcomingWindowEnd);
    context->agendaCoverageExpired = !covered;
    if (!covered) {
        lv_obj_t* unavailable = lv_label_create(context->agendaPanel);
        lv_label_set_text(unavailable,
            "Cached Agenda does not cover the current 30-day window. Sync to refresh.");
        lv_obj_set_width(unavailable, LV_PCT(100));
        lv_label_set_long_mode(unavailable, LV_LABEL_LONG_MODE_WRAP);
        return;
    }

    if (context->upcomingEvents.empty()) {
        lv_obj_t* empty = lv_label_create(context->agendaPanel);
        lv_label_set_text(empty, "No events in the next 30 days.");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        return;
    }

    for (const CalendarEvent& event : context->upcomingEvents) {
        lv_obj_t* card = lvgl_card_create(context->agendaPanel);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(card, 4, 0);
        lv_obj_set_scrollable(card, false);
        lv_obj_set_clickable(card, false);

        lv_obj_t* date = lv_label_create(card);
        lv_label_set_text(date, event.start);
        lv_obj_set_width(date, LV_PCT(100));
        lv_label_set_long_mode(date, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(date, lvgl_get_text_font(FONT_SIZE_SMALL), 0);

        lv_obj_t* summary = lv_label_create(card);
        lv_label_set_text(summary, event.summary);
        lv_obj_set_width(summary, LV_PCT(100));
        lv_label_set_long_mode(summary, LV_LABEL_LONG_MODE_WRAP);
    }
}

void onAgendaCoverageTimer(lv_timer_t* timer) {
    auto* context = static_cast<CalendarContext*>(lv_timer_get_user_data(timer));
    if (context->agendaPanel == nullptr || context->page != CalendarPage::Agenda ||
        context->dayAgendaVisible || context->settingsVisible ||
        context->agendaCoverageExpired) {
        return;
    }

    if (context->upcomingEventsGeneration != context->settingsGeneration.load() ||
        !calendarUpcomingWindowCoversNow(context->upcomingWindowStart,
                                         context->upcomingWindowEnd)) {
        refreshEventList(context);
    }
}

void onDayClicked(lv_event_t* event);

void setDaySelectedStyle(lv_obj_t* day, bool selected) {
    lv_obj_set_style_border_width(day, selected ? 2 : 0, LV_PART_MAIN);
    lv_obj_set_style_border_color(day, lvgl_theme_is_mono()
        ? lv_obj_get_style_text_color(lv_obj_get_parent(day), LV_PART_MAIN)
        : lv_theme_get_color_secondary(day), LV_PART_MAIN);
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

    std::vector<CalendarEvent> monthEvents;
    if (context->fullCalendarAvailable &&
        context->fullCalendarGeneration == context->settingsGeneration.load()) {
        calendarExpandEventsForMonth(context->events, context->displayYear,
                                     context->displayMonth, monthEvents);
    }

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

        lv_obj_t* number = lv_label_create(day);
        char numberText[3] {};
        snprintf(numberText, sizeof(numberText), "%d", dayNumber);
        lv_label_set_text(number, numberText);
        lv_obj_set_style_text_align(number, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        const lv_color_t textColor = lv_obj_get_style_text_color(context->monthGrid, LV_PART_MAIN);
        lv_obj_set_style_text_color(number, textColor, LV_PART_MAIN);

        char dateText[11] {};
        calendarFormatDateKey(dateText, sizeof(dateText),
            context->displayYear, context->displayMonth, dayNumber);
        bool hasEvent = false;
        for (size_t i = 0; i < monthEvents.size() && !hasEvent; ++i) {
            hasEvent = calendarEventOverlapsDay(monthEvents[i], dateText);
        }
        if (hasEvent) {
            lv_obj_t* marker = lv_obj_create(day);
            lv_obj_set_size(marker, 6, 6);
            lv_obj_set_style_radius(marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
            lv_obj_set_style_border_width(marker, 0, LV_PART_MAIN);
            lv_obj_set_style_bg_opa(marker, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(marker, lvgl_theme_is_mono()
                ? textColor : lv_theme_get_color_secondary(marker), LV_PART_MAIN);
        }
        const bool selected = context->selectedYear == context->displayYear &&
            context->selectedMonth == context->displayMonth &&
            context->selectedDay == dayNumber;
        setDaySelectedStyle(day, selected);
    }
}

void refreshMonthAgenda(CalendarContext* context) {
    if (context->selectedDayTitle == nullptr || context->selectedDayEvents == nullptr) return;

    char dateText[11] {};
    if (!calendarFormatDateKey(dateText, sizeof(dateText),
            context->selectedYear, context->selectedMonth, context->selectedDay)) {
        lv_label_set_text(context->selectedDayTitle, "Selected day");
        return;
    }
    lv_label_set_text(context->selectedDayTitle, dateText);

    lv_obj_clean(context->selectedDayEvents);
    if (!context->fullCalendarAvailable ||
        context->fullCalendarGeneration != context->settingsGeneration.load()) {
        lv_obj_t* unavailable = lv_label_create(context->selectedDayEvents);
        lv_label_set_text(unavailable, "Calendar data unavailable. Sync to download the calendar.");
        lv_obj_set_width(unavailable, LV_PCT(100));
        lv_label_set_long_mode(unavailable, LV_LABEL_LONG_MODE_WRAP);
        return;
    }

    std::vector<CalendarEvent> monthEvents;
    const bool expansionComplete = calendarExpandEventsForMonth(
        context->events, context->selectedYear, context->selectedMonth, monthEvents);
    size_t shownCount = 0;
    for (size_t i = 0; i < monthEvents.size(); ++i) {
        if (calendarEventOverlapsDay(monthEvents[i], dateText)) {
            ++shownCount;
        }
    }
    if (shownCount == 0) {
        lv_obj_t* empty = lv_label_create(context->selectedDayEvents);
        lv_label_set_text(empty, expansionComplete
            ? "No events scheduled for this day."
            : "No events found; some recurring events could not be expanded.");
        lv_obj_set_width(empty, LV_PCT(100));
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        return;
    }

    for (size_t i = 0; i < monthEvents.size(); ++i) {
        const CalendarEvent& event = monthEvents[i];
        if (!calendarEventOverlapsDay(event, dateText)) continue;

        lv_obj_t* card = lvgl_card_create(context->selectedDayEvents);
        lv_obj_set_width(card, LV_PCT(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(card, 2, LV_PART_MAIN);
        lv_obj_set_scrollable(card, false);
        lv_obj_set_clickable(card, false);

        lv_obj_t* time = lv_label_create(card);
        lv_label_set_text(time, event.start + 11);
        lv_obj_set_width(time, LV_PCT(100));
        lv_obj_set_style_text_font(time, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);

        lv_obj_t* summary = lv_label_create(card);
        lv_label_set_text(summary, event.summary);
        lv_obj_set_width(summary, LV_PCT(100));
        lv_label_set_long_mode(summary, LV_LABEL_LONG_MODE_WRAP);
    }
    if (!expansionComplete) {
        lv_obj_t* warning = lv_label_create(context->selectedDayEvents);
        lv_label_set_text(warning, "Some recurring events could not be expanded.");
        lv_obj_set_width(warning, LV_PCT(100));
        lv_label_set_long_mode(warning, LV_LABEL_LONG_MODE_WRAP);
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
    lv_group_focus_obj(context->selectedDayEvents);
}

void onBackToMonth(lv_event_t* event) {
    auto* context = static_cast<CalendarContext*>(lv_event_get_user_data(event));
    context->dayAgendaVisible = false;
    lv_obj_set_hidden(context->selectedDayPanel, true);
    lv_obj_set_hidden(context->monthPanel, false);
    lv_group_focus_obj(context->monthGrid);
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

void onStatusTimeout(lv_timer_t* timer) {
    auto* context = static_cast<CalendarContext*>(lv_timer_get_user_data(timer));
    if (context->statusTimer != timer) return;
    context->statusTimer = nullptr;
    context->statusShouldExpire = false;
    context->status[0] = '\0';
    if (context->statusLabel != nullptr) {
        lv_label_set_text(context->statusLabel, "");
        lv_obj_add_flag(context->statusLabel, LV_OBJ_FLAG_HIDDEN);
    }
}

void scheduleStatusTimeout(CalendarContext* context) {
    if (context->statusLabel == nullptr || context->status[0] == '\0') return;
    if (context->statusTimer != nullptr) {
        lv_timer_delete(context->statusTimer);
        context->statusTimer = nullptr;
    }
    context->statusTimer = lv_timer_create(onStatusTimeout, 5000, context);
    if (context->statusTimer != nullptr) {
        lv_timer_set_repeat_count(context->statusTimer, 1);
    }
}

void setStatusLocked(CalendarContext* context, const char* status, bool dismissAfterTimeout) {
    if (context->statusTimer != nullptr) {
        lv_timer_delete(context->statusTimer);
        context->statusTimer = nullptr;
    }
    copyText(context->status, sizeof(context->status), status);
    context->statusShouldExpire = dismissAfterTimeout;
    if (context->statusLabel != nullptr) {
        lv_label_set_text(context->statusLabel, context->status);
        if (context->status[0] == '\0') {
            lv_obj_add_flag(context->statusLabel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(context->statusLabel, LV_OBJ_FLAG_HIDDEN);
        }
        if (dismissAfterTimeout) scheduleStatusTimeout(context);
    }
}

void setStatus(CalendarContext* context, const char* status) {
    lvgl_lock();
    setStatusLocked(context, status, false);
    lvgl_unlock();
}

void setTransientStatus(CalendarContext* context, const char* status) {
    lvgl_lock();
    setStatusLocked(context, status, true);
    lvgl_unlock();
}

void setStatusForGeneration(CalendarContext* context, uint32_t generation,
                            const char* status) {
    lvgl_lock();
    const char* message = context->settingsGeneration.load() == generation
        ? status
        : "Sync discarded because calendar settings changed.";
    setStatusLocked(context, message, true);
    lvgl_unlock();
}

bool updateCalendarData(CalendarContext* context, uint32_t expectedGeneration,
                        bool updateFullCalendar, std::vector<CalendarEvent>&& events,
                        bool updateUpcoming, std::vector<CalendarEvent>&& upcomingEvents,
                        const char* upcomingWindowStart, const char* upcomingWindowEnd,
                        std::vector<CalendarEvent>& cachedEvents,
                        std::vector<CalendarEvent>& cachedUpcomingEvents,
                        char cachedWindowStart[17], char cachedWindowEnd[17],
                        bool& cachedFullCalendarAvailable) {
    if (context->settingsGeneration.load() != expectedGeneration) return false;
    lvgl_lock();
    if (context->settingsGeneration.load() != expectedGeneration) {
        lvgl_unlock();
        return false;
    }
    if (updateFullCalendar) {
        context->events = std::move(events);
        context->fullCalendarAvailable = true;
        context->fullCalendarGeneration = expectedGeneration;
        refreshMonth(context);
        refreshMonthAgenda(context);
    }
    if (updateUpcoming) {
        context->upcomingEvents = std::move(upcomingEvents);
        context->upcomingEventsGeneration = expectedGeneration;
        copyText(context->upcomingWindowStart, sizeof(context->upcomingWindowStart),
                 upcomingWindowStart);
        copyText(context->upcomingWindowEnd, sizeof(context->upcomingWindowEnd),
                 upcomingWindowEnd);
        refreshEventList(context);
    }
    cachedEvents = context->events;
    cachedUpcomingEvents = context->upcomingEvents;
    copyText(cachedWindowStart, 17, context->upcomingWindowStart);
    copyText(cachedWindowEnd, 17, context->upcomingWindowEnd);
    cachedFullCalendarAvailable = context->fullCalendarAvailable;
    lvgl_unlock();
    return true;
}

void saveSettings(CalendarContext* context) {
    const CalendarSyncSettings settings = getCalendarSyncSettings(context);
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
    preferences_put_string(preferences, "cacheIdentity", settings.cacheIdentity);
    preferences_put_string(preferences, "calendarUrl", settings.calendarUrl);
    preferences_put_string(preferences, "username", settings.username);
    preferences_put_string(preferences, "password", settings.password);
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
    const error_t identityResult = preferences_opt_string(preferences, "cacheIdentity",
        context->cacheIdentity, sizeof(context->cacheIdentity));
    if (identityResult == ERROR_NOT_FOUND ||
        (identityResult == ERROR_NONE && context->cacheIdentity[0] == '\0')) {
        createCacheIdentity(context->cacheIdentity, sizeof(context->cacheIdentity));
        preferences_put_string(preferences, "cacheIdentity", context->cacheIdentity);
    }
    preferences_close(preferences);
    if ((urlResult != ERROR_NONE && urlResult != ERROR_NOT_FOUND) ||
        (usernameResult != ERROR_NONE && usernameResult != ERROR_NOT_FOUND) ||
        (passwordResult != ERROR_NONE && passwordResult != ERROR_NOT_FOUND) ||
        (identityResult != ERROR_NONE && identityResult != ERROR_NOT_FOUND)) {
        copyText(context->status, sizeof(context->status), "Calendar settings could not be loaded completely.");
        return;
    }
    if (context->calendarUrl[0] != '\0') {
        copyText(context->status, sizeof(context->status), "Ready to sync the full calendar.");
    }
}

void calendarLoadCachedEvents(CalendarContext* context) {
    uint32_t cacheGeneration = 0;
    const CalendarSyncSettings settings = getCalendarSyncSettings(context, &cacheGeneration);
    if (settings.calendarUrl[0] == '\0') return;

    std::vector<CalendarEvent> events;
    std::vector<CalendarEvent> upcomingEvents;
    bool found = false;
    bool matchesSettings = false;
    bool fullCalendarAvailable = false;
    if (!calendarLoadCache(settings, events, upcomingEvents, fullCalendarAvailable,
                           context->upcomingWindowStart,
                           sizeof(context->upcomingWindowStart),
                           context->upcomingWindowEnd,
                           sizeof(context->upcomingWindowEnd),
                           found, matchesSettings)) {
        copyText(context->status, sizeof(context->status), "Saved calendar cache is invalid; sync to refresh.");
        return;
    }
    if (!found || !matchesSettings) return;

    context->events = std::move(events);
    context->upcomingEvents = std::move(upcomingEvents);
    context->fullCalendarAvailable = fullCalendarAvailable;
    context->fullCalendarGeneration = cacheGeneration;
    context->upcomingEventsGeneration = cacheGeneration;
    if (calendarUpcomingWindowCoversNow(context->upcomingWindowStart,
                                        context->upcomingWindowEnd)) {
        snprintf(context->status, sizeof(context->status),
            "Loaded %zu cached events. Sync to update.", context->events.size());
    } else {
        copyText(context->status, sizeof(context->status),
                 "Cached Agenda window expired. Sync to refresh.");
    }
    context->statusShouldExpire = true;
}

void calendarCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* context = static_cast<CalendarContext*>(userData);
    const CalendarSyncSettings settingsSnapshot = getCalendarSyncSettings(context);
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

    context->statusLabel = lv_label_create(parent);
    lv_obj_set_width(context->statusLabel, LV_PCT(100));
    lv_label_set_long_mode(context->statusLabel, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_font(context->statusLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_label_set_text(context->statusLabel, context->status);
    if (context->status[0] == '\0') {
        lv_obj_add_flag(context->statusLabel, LV_OBJ_FLAG_HIDDEN);
    }
    if (context->statusShouldExpire) scheduleStatusTimeout(context);

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
    if (lv_group_t* group = lv_group_get_default()) {
        lv_group_add_obj(group, context->selectedDayEvents);
    }
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
    if (lv_group_t* group = lv_group_get_default()) {
        lv_group_add_obj(group, context->agendaPanel);
    }

    refreshEventList(context);
    context->agendaCoverageTimer = lv_timer_create(onAgendaCoverageTimer, 60000, context);
    if (context->agendaCoverageTimer == nullptr) {
        setStatus(context, "Could not start the Agenda coverage timer.");
    }

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
    if (context->statusTimer != nullptr) {
        lv_timer_delete(context->statusTimer);
        context->statusTimer = nullptr;
    }
    if (context->agendaCoverageTimer != nullptr) {
        lv_timer_delete(context->agendaCoverageTimer);
        context->agendaCoverageTimer = nullptr;
    }
    context->agendaCoverageExpired = false;
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
        uint32_t syncGeneration = 0;
        const CalendarSyncSettings settings = getCalendarSyncSettings(context, &syncGeneration);
        CalendarSyncResult sync;
        const bool anySucceeded = calendarSyncCalendar(settings, sync);
        char message[sizeof(context->status)] {};
        bool cached = true;
        size_t downloadedCount = 0;
        bool stale = context->settingsGeneration.load() != syncGeneration;
        if (anySucceeded && !stale) {
            downloadedCount = sync.events.size();
            std::vector<CalendarEvent> cachedEvents;
            std::vector<CalendarEvent> cachedUpcomingEvents;
            char cachedWindowStart[17] {};
            char cachedWindowEnd[17] {};
            bool cachedFullCalendarAvailable = false;
            stale = !updateCalendarData(
                context, syncGeneration, sync.fullSucceeded, std::move(sync.events),
                sync.upcomingSucceeded, std::move(sync.upcomingEvents),
                sync.upcomingWindowStart, sync.upcomingWindowEnd,
                cachedEvents, cachedUpcomingEvents, cachedWindowStart, cachedWindowEnd,
                cachedFullCalendarAvailable);
            if (!stale && context->settingsGeneration.load() == syncGeneration) {
                cached = calendarSaveCache(settings, cachedEvents, cachedUpcomingEvents,
                                           cachedFullCalendarAvailable,
                                           cachedWindowStart, cachedWindowEnd);
                stale = context->settingsGeneration.load() != syncGeneration;
            } else if (!stale) {
                stale = true;
            }
        }

        if (stale) {
            copyText(message, sizeof(message),
                     "Sync discarded because calendar settings changed.");
        } else if (sync.fullSucceeded && sync.upcomingSucceeded) {
            if (cached) {
                snprintf(message, sizeof(message), "Downloaded %zu calendar events.",
                         downloadedCount);
            } else {
                copyText(message, sizeof(message),
                         "Calendar downloaded, but its local cache could not be saved.");
            }
        } else if (sync.upcomingSucceeded) {
            snprintf(message, sizeof(message), "Agenda updated; full refresh failed: %.38s%s",
                     sync.fullError, cached ? "" : "; cache not saved.");
        } else if (sync.fullSucceeded) {
            snprintf(message, sizeof(message), "Month updated; agenda refresh failed: %.36s%s",
                     sync.upcomingError, cached ? "" : "; cache not saved.");
        } else {
            copyText(message, sizeof(message), sync.fullError);
        }
        if (stale) {
            setTransientStatus(context, message);
        } else {
            setStatusForGeneration(context, syncGeneration, message);
        }
        lvgl_lock();
        refreshEventList(context);
        lvgl_unlock();
    }
}
