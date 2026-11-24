#pragma once

#include "CalendarData.h"

#include <vector>

struct CalendarSyncSettings {
    char calendarUrl[256] {};
    char username[96] {};
    char password[96] {};
    char cacheIdentity[17] {};
};

struct CalendarSyncResult {
    bool fullSucceeded = false;
    bool upcomingSucceeded = false;
    std::vector<CalendarEvent> events;
    std::vector<CalendarEvent> upcomingEvents;
    char upcomingWindowStart[17] {};
    char upcomingWindowEnd[17] {};
    char fullError[96] {};
    char upcomingError[96] {};
};

bool calendarSyncCalendar(const CalendarSyncSettings& settings, CalendarSyncResult& sync);
bool calendarUpcomingWindowCoversNow(const char* windowStart, const char* windowEnd);
bool calendarSaveCache(const CalendarSyncSettings& settings,
                       const std::vector<CalendarEvent>& events,
                       const std::vector<CalendarEvent>& upcomingEvents,
                       bool fullCalendarAvailable,
                       bool fullCalendarHasTiming,
                       const char* upcomingWindowStart,
                       const char* upcomingWindowEnd);
bool calendarLoadCache(const CalendarSyncSettings& settings,
                       std::vector<CalendarEvent>& events,
                       std::vector<CalendarEvent>& upcomingEvents,
                       bool& fullCalendarAvailable,
                       bool& fullCalendarHasTiming,
                       char* upcomingWindowStart, size_t upcomingWindowStartSize,
                       char* upcomingWindowEnd, size_t upcomingWindowEndSize,
                       bool& found, bool& matchesSettings);
