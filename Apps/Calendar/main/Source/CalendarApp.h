#pragma once

#include <app/instance.h>
#include <lvgl.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/concurrent/task_event_group.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>

struct CalendarEvent {
    char start[48] {};
    char summary[112] {};
};

struct CalendarContext {
    static constexpr size_t MAX_EVENTS = 24;

    AppInstanceId appInstanceId = 0;
    WindowId window = 0;
    TaskEventGroup* eventGroup = nullptr;
    uint32_t requestBit = 0;
    std::atomic<uint32_t> requests {0};

    std::mutex settingsMutex;
    char calendarUrl[256] {};
    char username[96] {};
    char password[96] {};
    char status[96] = "Configure a CalDAV calendar to get started.";
    std::array<CalendarEvent, MAX_EVENTS> events {};
    size_t eventCount = 0;

    bool settingsVisible = false;
    lv_obj_t* agendaPanel = nullptr;
    lv_obj_t* settingsPanel = nullptr;
    lv_obj_t* eventList = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* urlInput = nullptr;
    lv_obj_t* usernameInput = nullptr;
    lv_obj_t* passwordInput = nullptr;
};

void calendarLoadSettings(CalendarContext* context);
void calendarCreateWidgets(lv_obj_t* parent, void* userData);
void calendarDestroyWidgets(void* userData);
void calendarProcessRequests(CalendarContext* context);
