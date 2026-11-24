#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct CalendarTimeZoneTransition {
    char start[16] {};
    int32_t offsetFrom = 0;
    int32_t offsetTo = 0;
    int8_t recurrenceMonth = 0;
    int8_t recurrenceMonthDay = 0;
    int8_t recurrenceWeekday = -1;
    int8_t recurrenceOrdinal = 0;
    bool repeatsYearly = false;
};

struct CalendarEvent {
    char uid[256] {};
    char occurrenceId[40] {};
    char startDate[11] {};
    char endDateExclusive[11] {};
    char start[48] {};
    char summary[112] {};
    char timingStart[17] {};
    char timingEnd[17] {};
    char timingDuration[40] {};
    char timingOccurrenceStart[17] {};
    // Offset at the original DTEND when it uses a different time zone.
    int32_t timingEndOffset = 0;
    bool timingEndOffsetValid = false;
    char recurrenceRule[192] {};
    char recurrenceLocalId[20] {};
    char recurrenceTimeZone[96] {};
    char excludedDates[128] {};
    CalendarTimeZoneTransition timeZoneTransitions[4] {};
    uint8_t timeZoneTransitionCount = 0;
    bool recurrenceOverride = false;
    bool recurrenceThisAndFuture = false;
    bool cancelled = false;
    bool recurrenceUnsupported = false;
    bool excludedDatesIncomplete = false;
    bool timeZoneTransitionsIncomplete = false;
    bool timingIncomplete = false;
};

bool calendarFormatDateKey(char* output, std::size_t size, int year, int month, int day);
bool calendarEventOverlapsDay(const CalendarEvent& event, const char* date);
bool calendarParseResponse(const std::string& xml, std::vector<CalendarEvent>& events);
bool calendarExpandEventsForMonth(const std::vector<CalendarEvent>& events,
                                 int year, int month,
                                 std::vector<CalendarEvent>& monthEvents);
bool calendarExpandEventsForDateRange(const std::vector<CalendarEvent>& events,
                                     const char* startDate, const char* endDateExclusive,
                                     std::vector<CalendarEvent>& rangeEvents);
bool calendarExpandEventsForInterval(const std::vector<CalendarEvent>& events,
                                    int64_t start, int64_t end,
                                    std::vector<CalendarEvent>& intervalEvents,
                                    int64_t* nextChange = nullptr);
