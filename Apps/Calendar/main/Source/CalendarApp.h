#pragma once

#include <app/instance.h>
#include "CalendarSync.h"
#include <lvgl.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/concurrent/task_event_group.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

enum class CalendarPage {
    Agenda,
    Month,
};

struct CalendarContext {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;
    TaskEventGroup* eventGroup = nullptr;
    uint32_t requestBit = 0;
    std::atomic<uint32_t> requests {0};
    std::atomic<uint32_t> settingsGeneration {0};

    std::mutex settingsMutex;
    char calendarUrl[256] {};
    char username[96] {};
    char password[96] {};
    char cacheIdentity[17] {};
    char status[96] = "Configure a CalDAV calendar to get started.";
    std::vector<CalendarEvent> events;
    std::vector<CalendarEvent> upcomingEvents;
    bool fullCalendarAvailable = false;
    uint32_t fullCalendarGeneration = 0;
    uint32_t upcomingEventsGeneration = 0;
    char upcomingWindowStart[17] {};
    char upcomingWindowEnd[17] {};
    bool statusShouldExpire = false;

    bool settingsVisible = false;
    bool dayAgendaVisible = false;
    CalendarPage page = CalendarPage::Agenda;
    int selectedYear = 0;
    int selectedMonth = 0;
    int selectedDay = 0;
    int displayYear = 0;
    int displayMonth = 0;
    lv_obj_t* agendaPanel = nullptr;
    lv_obj_t* monthPanel = nullptr;
    lv_obj_t* monthTitle = nullptr;
    lv_obj_t* monthGrid = nullptr;
    lv_obj_t* selectedDayPanel = nullptr;
    lv_obj_t* selectedDayTitle = nullptr;
    lv_obj_t* selectedDayEvents = nullptr;
    lv_obj_t* settingsPanel = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_timer_t* statusTimer = nullptr;
    lv_timer_t* agendaCoverageTimer = nullptr;
    bool agendaCoverageExpired = false;
    int64_t agendaWindowStart = 0;
    int64_t agendaNextRefresh = 0;
    bool agendaUsesLocalCalendar = false;
    lv_obj_t* urlInput = nullptr;
    lv_obj_t* usernameInput = nullptr;
    lv_obj_t* passwordInput = nullptr;
};

void calendarLoadSettings(CalendarContext* context);
void calendarLoadCachedEvents(CalendarContext* context);
void calendarCreateWidgets(lv_obj_t* parent, void* userData);
void calendarDestroyWidgets(void* userData);
void calendarProcessRequests(CalendarContext* context);
