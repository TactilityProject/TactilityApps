#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct CalendarEvent {
    char uid[256] {};
    char occurrenceId[40] {};
    char startDate[11] {};
    char endDateExclusive[11] {};
    char start[48] {};
    char summary[112] {};
    char recurrenceRule[192] {};
    char recurrenceLocalId[20] {};
    char excludedDates[128] {};
    bool recurrenceOverride = false;
    bool cancelled = false;
    bool recurrenceUnsupported = false;
};

bool calendarFormatDateKey(char* output, std::size_t size, int year, int month, int day);
bool calendarEventOverlapsDay(const CalendarEvent& event, const char* date);
bool calendarParseResponse(const std::string& xml, std::vector<CalendarEvent>& events);
bool calendarExpandEventsForMonth(const std::vector<CalendarEvent>& events,
                                 int year, int month,
                                 std::vector<CalendarEvent>& monthEvents);
