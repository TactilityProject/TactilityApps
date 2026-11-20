#include "CalendarData.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

void copyText(char* target, size_t size, const char* source) {
    if (size == 0) return;
    snprintf(target, size, "%s", source == nullptr ? "" : source);
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

int daysInMonth(int year, int month) {
    static constexpr int DAYS_PER_MONTH[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month != 2) return DAYS_PER_MONTH[month - 1];
    const bool leapYear = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return leapYear ? 29 : 28;
}

bool formatIcsDateKey(const std::string& value, char output[11]) {
    if (value.size() < 8) return false;
    for (size_t i = 0; i < 8; ++i) {
        if (value[i] < '0' || value[i] > '9') return false;
    }

    const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
        (value[2] - '0') * 10 + value[3] - '0';
    const int month = (value[4] - '0') * 10 + value[5] - '0';
    const int day = (value[6] - '0') * 10 + value[7] - '0';
    return day <= daysInMonth(year, month) &&
        calendarFormatDateKey(output, 11, year, month, day);
}

bool incrementDateKey(const char* date, char output[11]) {
    const int year = (date[0] - '0') * 1000 + (date[1] - '0') * 100 +
        (date[2] - '0') * 10 + date[3] - '0';
    int month = (date[5] - '0') * 10 + date[6] - '0';
    int day = (date[8] - '0') * 10 + date[9] - '0';
    if (year < 1000 || year > 9999 || month < 1 || month > 12 ||
        day < 1 || day > daysInMonth(year, month)) {
        return false;
    }

    if (++day > daysInMonth(year, month)) {
        day = 1;
        if (++month > 12) {
            month = 1;
            if (year == 9999) return false;
            return calendarFormatDateKey(output, 11, year + 1, month, day);
        }
    }
    return calendarFormatDateKey(output, 11, year, month, day);
}

bool formatEventDateRange(const std::string& start, const std::string& end, CalendarEvent& event) {
    if (!formatIcsDateKey(start, event.startDate)) return false;
    if (end.empty()) {
        return incrementDateKey(event.startDate, event.endDateExclusive);
    }
    if (!formatIcsDateKey(end, event.endDateExclusive)) return false;

    const bool isDateTime = end.size() >= 15 && end[8] == 'T';
    const bool endsAtMidnight = isDateTime &&
        end[9] == '0' && end[10] == '0' &&
        end[11] == '0' && end[12] == '0' &&
        end[13] == '0' && end[14] == '0';
    if (isDateTime && !endsAtMidnight) {
        char nextDate[11] {};
        if (!incrementDateKey(event.endDateExclusive, nextDate)) return false;
        copyText(event.endDateExclusive, sizeof(event.endDateExclusive), nextDate);
    }
    return true;
}

struct IcsDateTime {
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    bool dateOnly = false;
    bool utc = false;
};

struct TimeZoneTransition {
    std::string tzid;
    std::string start;
    int offsetFrom = 0;
    int offsetTo = 0;
    int recurrenceMonth = 0;
    int recurrenceMonthDay = 0;
    int recurrenceWeekday = -1;
    int recurrenceOrdinal = 0;
    bool repeatsYearly = false;
};

bool parseIcsDateTime(const std::string& value, IcsDateTime& result) {
    result = {};
    if (value.size() != 8 && value.size() != 15 &&
        !(value.size() == 16 && value.back() == 'Z')) {
        return false;
    }
    const bool dateOnly = value.size() == 8;
    if (!dateOnly && value[8] != 'T') return false;
    const size_t digitCount = dateOnly ? 8 : 15;
    for (size_t i = 0; i < digitCount; ++i) {
        if (i == 8 && !dateOnly) continue;
        if (value[i] < '0' || value[i] > '9') return false;
    }

    result.year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
        (value[2] - '0') * 10 + value[3] - '0';
    result.month = (value[4] - '0') * 10 + value[5] - '0';
    result.day = (value[6] - '0') * 10 + value[7] - '0';
    result.dateOnly = dateOnly;
    if (result.year < 1000 || result.year > 9999 || result.month < 1 ||
        result.month > 12 || result.day < 1 ||
        result.day > daysInMonth(result.year, result.month)) {
        return false;
    }
    if (!dateOnly) {
        result.hour = (value[9] - '0') * 10 + value[10] - '0';
        result.minute = (value[11] - '0') * 10 + value[12] - '0';
        result.second = (value[13] - '0') * 10 + value[14] - '0';
        if (result.hour > 23 || result.minute > 59 || result.second > 60) return false;
        result.utc = value.size() == 16;
    }
    return true;
}

int64_t civilSeconds(const IcsDateTime& dateTime) {
    int year = dateTime.year;
    const unsigned month = static_cast<unsigned>(dateTime.month);
    const unsigned day = static_cast<unsigned>(dateTime.day);
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
    const unsigned adjustedMonth = month > 2 ? month - 3 : month + 9;
    const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
    const unsigned dayOfEra =
        yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
    const int64_t days = static_cast<int64_t>(era) * 146097 +
        static_cast<int64_t>(dayOfEra) - 719468;
    return days * 86400 + dateTime.hour * 3600 + dateTime.minute * 60 + dateTime.second;
}

int weekdayForDate(int year, int month, int day) {
    IcsDateTime dateTime {};
    dateTime.year = year;
    dateTime.month = month;
    dateTime.day = day;
    int weekday = static_cast<int>((civilSeconds(dateTime) / 86400 + 4) % 7);
    if (weekday < 0) weekday += 7;
    return weekday;
}

bool parseTimezoneOffset(const std::string& value, int& offset) {
    if ((value.size() != 5 && value.size() != 7) ||
        (value[0] != '+' && value[0] != '-')) {
        return false;
    }
    for (size_t i = 1; i < value.size(); ++i) {
        if (!isdigit(static_cast<unsigned char>(value[i]))) return false;
    }
    const int hours = (value[1] - '0') * 10 + value[2] - '0';
    const int minutes = (value[3] - '0') * 10 + value[4] - '0';
    const int seconds = value.size() == 7
        ? (value[5] - '0') * 10 + value[6] - '0'
        : 0;
    if (hours > 23 || minutes > 59 || seconds > 59) return false;
    const int magnitude = hours * 3600 + minutes * 60 + seconds;
    offset = value[0] == '-' ? -magnitude : magnitude;
    return true;
}

int parseIcsInteger(const std::string& value) {
    if (value.empty()) return 0;
    char* end = nullptr;
    const long parsed = strtol(value.c_str(), &end, 10);
    return end == value.c_str() || *end != '\0' ||
        parsed < -366 || parsed > 366 ? 0 : static_cast<int>(parsed);
}

void parseTimezoneRecurrence(const std::string& value, TimeZoneTransition& transition) {
    size_t position = 0;
    while (position < value.size()) {
        size_t end = value.find(';', position);
        if (end == std::string::npos) end = value.size();
        const size_t equals = value.find('=', position);
        if (equals != std::string::npos && equals < end) {
            const std::string key = value.substr(position, equals - position);
            const std::string ruleValue = value.substr(equals + 1, end - equals - 1);
            if (equalsIgnoreCase(key, "FREQ")) {
                transition.repeatsYearly = equalsIgnoreCase(ruleValue, "YEARLY");
            } else if (equalsIgnoreCase(key, "BYMONTH")) {
                transition.recurrenceMonth = parseIcsInteger(ruleValue);
            } else if (equalsIgnoreCase(key, "BYMONTHDAY")) {
                transition.recurrenceMonthDay = parseIcsInteger(ruleValue);
            } else if (equalsIgnoreCase(key, "BYDAY")) {
                size_t weekdayStart = ruleValue.size();
                while (weekdayStart > 0 &&
                       ruleValue[weekdayStart - 1] >= 'A' &&
                       ruleValue[weekdayStart - 1] <= 'Z') {
                    --weekdayStart;
                }
                if (weekdayStart < ruleValue.size()) {
                    const std::string weekday = ruleValue.substr(weekdayStart);
                    static const char* WEEKDAYS[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
                    for (int i = 0; i < 7; ++i) {
                        if (weekday == WEEKDAYS[i]) transition.recurrenceWeekday = i;
                    }
                    transition.recurrenceOrdinal =
                        parseIcsInteger(ruleValue.substr(0, weekdayStart));
                }
            }
        }
        position = end + 1;
    }
}

std::vector<TimeZoneTransition> parseTimeZoneTransitions(const std::string& unfolded) {
    std::vector<TimeZoneTransition> transitions;
    bool inTimeZone = false;
    bool inObservance = false;
    bool hasOffsetFrom = false;
    bool hasOffsetTo = false;
    std::string tzid;
    TimeZoneTransition transition {};
    size_t position = 0;
    while (position <= unfolded.size()) {
        size_t end = unfolded.find('\n', position);
        if (end == std::string::npos) end = unfolded.size();
        const std::string line = unfolded.substr(position, end - position);
        position = end + 1;

        if (equalsIgnoreCase(line, "BEGIN:VTIMEZONE")) {
            inTimeZone = true;
            inObservance = false;
            tzid.clear();
        } else if (inTimeZone &&
                   (equalsIgnoreCase(line, "BEGIN:STANDARD") ||
                    equalsIgnoreCase(line, "BEGIN:DAYLIGHT"))) {
            inObservance = true;
            transition = {};
            transition.tzid = tzid;
            hasOffsetFrom = false;
            hasOffsetTo = false;
        } else if (inObservance &&
                   (equalsIgnoreCase(line, "END:STANDARD") ||
                    equalsIgnoreCase(line, "END:DAYLIGHT"))) {
            if (transition.tzid.empty()) transition.tzid = tzid;
            if (!transition.tzid.empty() && !transition.start.empty() &&
                hasOffsetFrom && hasOffsetTo) {
                transitions.push_back(transition);
            }
            inObservance = false;
        } else if (equalsIgnoreCase(line, "END:VTIMEZONE")) {
            inTimeZone = false;
            inObservance = false;
        } else if (inTimeZone) {
            const size_t colon = line.find(':');
            if (colon != std::string::npos) {
                const std::string property = line.substr(0, colon);
                const std::string value = line.substr(colon + 1);
                if (!inObservance && equalsIgnoreCase(property, "TZID")) {
                    tzid = value;
                } else if (inObservance && equalsIgnoreCase(property, "DTSTART")) {
                    transition.start = value;
                    transition.tzid = tzid;
                } else if (inObservance && equalsIgnoreCase(property, "TZOFFSETFROM")) {
                    hasOffsetFrom = parseTimezoneOffset(value, transition.offsetFrom);
                } else if (inObservance && equalsIgnoreCase(property, "TZOFFSETTO")) {
                    hasOffsetTo = parseTimezoneOffset(value, transition.offsetTo);
                } else if (inObservance && equalsIgnoreCase(property, "RRULE")) {
                    parseTimezoneRecurrence(value, transition);
                }
            }
        }
        if (end == unfolded.size()) break;
    }
    return transitions;
}

bool transitionDate(const TimeZoneTransition& transition, int year, IcsDateTime& dateTime) {
    IcsDateTime start {};
    if (!parseIcsDateTime(transition.start, start) || start.dateOnly) return false;
    int month = transition.recurrenceMonth == 0 ? start.month : transition.recurrenceMonth;
    if (month < 1 || month > 12) return false;
    int day = start.day;
    if (transition.recurrenceMonthDay != 0) {
        day = transition.recurrenceMonthDay;
        if (day < 0) day = daysInMonth(year, month) + day + 1;
    } else if (transition.recurrenceWeekday >= 0 &&
               transition.recurrenceOrdinal != 0) {
        if (transition.recurrenceOrdinal > 0) {
            const int firstWeekday = weekdayForDate(year, month, 1);
            day = 1 + (transition.recurrenceWeekday - firstWeekday + 7) % 7 +
                (transition.recurrenceOrdinal - 1) * 7;
        } else {
            const int lastDay = daysInMonth(year, month);
            const int lastWeekday = weekdayForDate(year, month, lastDay);
            day = lastDay - (lastWeekday - transition.recurrenceWeekday + 7) % 7 +
                (transition.recurrenceOrdinal + 1) * 7;
        }
    }
    if (day < 1 || day > daysInMonth(year, month)) return false;
    if (transition.recurrenceWeekday >= 0 &&
        transition.recurrenceMonthDay != 0 &&
        weekdayForDate(year, month, day) != transition.recurrenceWeekday) {
        return false;
    }
    dateTime = start;
    dateTime.year = year;
    dateTime.month = month;
    dateTime.day = day;
    dateTime.utc = false;
    return true;
}

bool timeZoneOffsetAt(const std::string& tzid, const IcsDateTime& dateTime,
                      const std::vector<TimeZoneTransition>& transitions, int& offset) {
    bool hasFallback = false;
    int fallbackOffset = 0;
    int fallbackYear = 10000;
    bool hasLatest = false;
    int64_t latestTransition = 0;
    int latestOffset = 0;
    const int64_t target = civilSeconds(dateTime);
    for (const TimeZoneTransition& transition : transitions) {
        if (transition.tzid != tzid) continue;
        IcsDateTime start {};
        if (!parseIcsDateTime(transition.start, start) || start.dateOnly) continue;
        if (!hasFallback || start.year < fallbackYear) {
            hasFallback = true;
            fallbackYear = start.year;
            fallbackOffset = transition.offsetFrom;
        }

        const auto considerTransition = [&](const IcsDateTime& at) {
            const int64_t localTime = civilSeconds(at);
            if (localTime <= target && (!hasLatest || localTime > latestTransition)) {
                hasLatest = true;
                latestTransition = localTime;
                latestOffset = transition.offsetTo;
            }
        };
        considerTransition(start);
        if (transition.repeatsYearly) {
            const int firstYear = std::max(start.year, dateTime.year - 1);
            for (int year = firstYear; year <= dateTime.year; ++year) {
                if (year == start.year) continue;
                IcsDateTime repeated {};
                if (transitionDate(transition, year, repeated)) {
                    considerTransition(repeated);
                }
            }
        }
    }
    if (hasLatest) {
        offset = latestOffset;
        return true;
    }
    if (hasFallback) {
        offset = fallbackOffset;
        return true;
    }
    return false;
}

std::string timezoneIdFromParameters(const std::string& parameters) {
    size_t position = 0;
    while (position < parameters.size()) {
        while (position < parameters.size() && parameters[position] == ';') ++position;
        size_t end = parameters.find(';', position);
        if (end == std::string::npos) end = parameters.size();
        const std::string parameter = parameters.substr(position, end - position);
        if (startsWithIgnoreCase(parameter, "TZID=")) {
            std::string tzid = parameter.substr(5);
            if (tzid.size() >= 2 && tzid.front() == '"' && tzid.back() == '"') {
                tzid = tzid.substr(1, tzid.size() - 2);
            }
            return tzid;
        }
        position = end + 1;
    }
    return "";
}

bool formatOccurrenceId(const std::string& value, const std::string& parameters,
                        const std::vector<TimeZoneTransition>& transitions,
                        char* output, size_t outputSize) {
    IcsDateTime dateTime {};
    if (!parseIcsDateTime(value, dateTime)) return false;
    if (dateTime.dateOnly) {
        snprintf(output, outputSize, "D%04d%02d%02d",
                 dateTime.year, dateTime.month, dateTime.day);
        return true;
    }

    const std::string tzid = timezoneIdFromParameters(parameters);
    if (dateTime.utc) {
        snprintf(output, outputSize, "T%lld",
                 static_cast<long long>(civilSeconds(dateTime)));
        return true;
    }
    if (!tzid.empty()) {
        int offset = 0;
        if (timeZoneOffsetAt(tzid, dateTime, transitions, offset)) {
            snprintf(output, outputSize, "T%lld",
                     static_cast<long long>(civilSeconds(dateTime) - offset));
            return true;
        }

        uint64_t hash = 14695981039346656037ULL;
        for (const unsigned char character : tzid) {
            hash = (hash ^ character) * 1099511628211ULL;
        }
        snprintf(output, outputSize, "L%016llx%04d%02d%02dT%02d%02d%02d",
                 static_cast<unsigned long long>(hash), dateTime.year, dateTime.month,
                 dateTime.day, dateTime.hour, dateTime.minute, dateTime.second);
        return true;
    }
    snprintf(output, outputSize, "F%04d%02d%02dT%02d%02d%02d",
             dateTime.year, dateTime.month, dateTime.day,
             dateTime.hour, dateTime.minute, dateTime.second);
    return true;
}

bool formatRecurrenceLocalId(const std::string& value, char* output, size_t outputSize) {
    IcsDateTime dateTime {};
    if (!parseIcsDateTime(value, dateTime)) return false;
    if (dateTime.dateOnly) {
        snprintf(output, outputSize, "D%04d%02d%02d",
                 dateTime.year, dateTime.month, dateTime.day);
    } else {
        snprintf(output, outputSize, "%c%04d%02d%02dT%02d%02d%02d",
                 dateTime.utc ? 'U' : 'L',
                 dateTime.year, dateTime.month, dateTime.day,
                 dateTime.hour, dateTime.minute, dateTime.second);
    }
    return true;
}

bool appendExcludedDates(std::string& excludedDates, const std::string& value) {
    size_t position = 0;
    while (position < value.size()) {
        size_t end = value.find(',', position);
        if (end == std::string::npos) end = value.size();
        char localId[20] {};
        if (!formatRecurrenceLocalId(value.substr(position, end - position),
                                     localId, sizeof(localId))) {
            return false;
        }
        const size_t extraLength = strlen(localId) + (excludedDates.empty() ? 0 : 1);
        if (excludedDates.size() + extraLength >= 128) return false;
        if (!excludedDates.empty()) excludedDates.push_back(',');
        excludedDates.append(localId);
        position = end + 1;
    }
    return true;
}

bool eventOverlapsDay(const CalendarEvent& event, const char* date) {
    return strncmp(event.startDate, date, 10) <= 0 &&
        strncmp(date, event.endDateExclusive, 10) < 0;
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
    const std::vector<TimeZoneTransition> timeZoneTransitions =
        parseTimeZoneTransitions(unfolded);

    bool inEvent = false;
    size_t componentDepth = 0;
    bool cancelled = false;
    std::string uid;
    std::string start;
    std::string endValue;
    std::string startParameters;
    std::string recurrenceId;
    std::string recurrenceParameters;
    std::string summary;
    std::string recurrenceRule;
    std::string excludedDates;
    bool recurrenceUnsupported = false;
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
            endValue.clear();
            startParameters.clear();
            recurrenceId.clear();
            recurrenceParameters.clear();
            summary.clear();
            recurrenceRule.clear();
            excludedDates.clear();
            recurrenceUnsupported = false;
        } else if (inEvent && startsWithIgnoreCase(line, "BEGIN:")) {
            ++componentDepth;
        } else if (inEvent && equalsIgnoreCase(line, "END:VEVENT") && componentDepth == 1) {
            if ((!cancelled || !recurrenceId.empty()) &&
                (!start.empty() || !recurrenceId.empty())) {
                CalendarEvent event {};
                copyText(event.uid, sizeof(event.uid), uid.c_str());
                const std::string& occurrenceValue = recurrenceId.empty() ? start : recurrenceId;
                const std::string& occurrenceParameters =
                    recurrenceId.empty() ? startParameters : recurrenceParameters;
                const std::string& eventStart = start.empty() ? recurrenceId : start;
                if (formatEventDateRange(eventStart, endValue, event) &&
                    formatOccurrenceId(occurrenceValue, occurrenceParameters,
                                       timeZoneTransitions, event.occurrenceId,
                                       sizeof(event.occurrenceId)) &&
                    formatRecurrenceLocalId(occurrenceValue, event.recurrenceLocalId,
                                            sizeof(event.recurrenceLocalId))) {
                    if (!cancelled) {
                        formatStart(eventStart, startParameters, event.start, sizeof(event.start));
                    }
                    const std::string displaySummary = summary.empty() ? "Untitled event" : unescapeIcs(summary);
                    copyText(event.summary, sizeof(event.summary), displaySummary.c_str());
                    copyText(event.recurrenceRule, sizeof(event.recurrenceRule),
                             recurrenceRule.c_str());
                    copyText(event.excludedDates, sizeof(event.excludedDates),
                             excludedDates.c_str());
                    event.recurrenceOverride = !recurrenceId.empty();
                    event.cancelled = cancelled;
                    event.recurrenceUnsupported = recurrenceUnsupported ||
                        recurrenceRule.size() >= sizeof(event.recurrenceRule);
                    events.push_back(event);
                }
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
            } else if (equalsIgnoreCase(property, "RECURRENCE-ID")) {
                recurrenceId = value;
                recurrenceParameters = parameters;
            } else if (equalsIgnoreCase(property, "DTEND")) {
                endValue = value;
            } else if (equalsIgnoreCase(property, "UID")) {
                uid = value;
            } else if (equalsIgnoreCase(property, "SUMMARY")) {
                summary = value;
            } else if (equalsIgnoreCase(property, "RRULE")) {
                recurrenceRule = value;
            } else if (equalsIgnoreCase(property, "EXDATE")) {
                if (!appendExcludedDates(excludedDates, value)) {
                    recurrenceUnsupported = true;
                }
            } else if (equalsIgnoreCase(property, "RDATE")) {
                recurrenceUnsupported = true;
            } else if (equalsIgnoreCase(property, "STATUS") && equalsIgnoreCase(value, "CANCELLED")) {
                cancelled = true;
            }
        }
        if (end == unfolded.size()) break;
    }
}

struct XmlTag {
    std::string name;
    std::string localName;
    size_t start = 0;
    size_t end = 0;
    bool closing = false;
    bool selfClosing = false;
};

struct XmlElement {
    std::string name;
    size_t start = 0;
    size_t contentStart = 0;
    size_t contentEnd = 0;
    size_t end = 0;
    bool selfClosing = false;
};

bool nextXmlTag(const std::string& xml, size_t& position, XmlTag& tag, bool& found) {
    found = false;
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

        const size_t tagStart = position;
        size_t nameStart = position + 1;
        const char marker = nameStart < xml.size() ? xml[nameStart] : '\0';
        if (marker == '?' || marker == '!') {
            const size_t tagEnd = xml.find('>', nameStart + 1);
            if (tagEnd == std::string::npos) return false;
            position = tagEnd + 1;
            continue;
        }

        const bool closing = marker == '/';
        if (closing) ++nameStart;
        const size_t nameEnd = xml.find_first_of(" \t\r\n/>", nameStart);
        if (nameEnd == std::string::npos || nameEnd == nameStart) return false;

        char quote = '\0';
        size_t tagEnd = nameEnd;
        for (; tagEnd < xml.size(); ++tagEnd) {
            const char character = xml[tagEnd];
            if (quote != '\0') {
                if (character == quote) quote = '\0';
            } else if (character == '"' || character == '\'') {
                quote = character;
            } else if (character == '>') {
                break;
            }
        }
        if (tagEnd == xml.size() || quote != '\0') return false;

        tag.name = xml.substr(nameStart, nameEnd - nameStart);
        const size_t namespaceSeparator = tag.name.rfind(':');
        tag.localName = tag.name.substr(
            namespaceSeparator == std::string::npos ? 0 : namespaceSeparator + 1);
        tag.start = tagStart;
        tag.end = tagEnd + 1;
        tag.closing = closing;
        size_t lastCharacter = tagEnd;
        while (lastCharacter > nameEnd &&
               (xml[lastCharacter - 1] == ' ' || xml[lastCharacter - 1] == '\t' ||
                xml[lastCharacter - 1] == '\r' || xml[lastCharacter - 1] == '\n')) {
            --lastCharacter;
        }
        tag.selfClosing = !closing && lastCharacter > nameEnd && xml[lastCharacter - 1] == '/';
        position = tag.end;
        found = true;
        return true;
    }
    return true;
}

bool findElementEnd(const std::string& xml, const XmlTag& opening,
                    size_t& contentEnd, size_t& elementEnd) {
    if (opening.closing || opening.selfClosing) return false;
    size_t depth = 1;
    size_t position = opening.end;
    XmlTag tag;
    bool found = false;
    while (true) {
        if (!nextXmlTag(xml, position, tag, found)) return false;
        if (!found) return false;
        if (tag.name != opening.name) continue;
        if (tag.closing) {
            if (--depth == 0) {
                contentEnd = tag.start;
                elementEnd = tag.end;
                return true;
            }
        } else if (!tag.selfClosing) {
            ++depth;
        }
    }
}

bool findNextElement(const std::string& xml, size_t begin, size_t end,
                     const char* localName, XmlElement& element, bool& found) {
    found = false;
    size_t position = begin;
    XmlTag tag;
    bool hasTag = false;
    while (position < end) {
        if (!nextXmlTag(xml, position, tag, hasTag)) return false;
        if (!hasTag) return true;
        if (tag.start >= end) return true;
        if (tag.closing || tag.localName != localName) continue;

        element.name = tag.name;
        element.start = tag.start;
        element.contentStart = tag.end;
        element.selfClosing = tag.selfClosing;
        if (tag.selfClosing) {
            element.contentEnd = tag.end;
            element.end = tag.end;
        } else if (!findElementEnd(xml, tag, element.contentEnd, element.end)) {
            return false;
        }
        if (element.end > end) return false;
        found = true;
        return true;
    }
    return true;
}

bool findDirectChild(const std::string& xml, size_t begin, size_t end,
                     const char* localName, XmlElement& element, bool& found) {
    found = false;
    size_t position = begin;
    size_t depth = 0;
    XmlTag tag;
    bool hasTag = false;
    while (position < end) {
        if (!nextXmlTag(xml, position, tag, hasTag)) return false;
        if (!hasTag || tag.start >= end) return true;
        if (tag.closing) {
            if (depth > 0) --depth;
            continue;
        }
        if (depth == 0 && tag.localName == localName) {
            element.name = tag.name;
            element.start = tag.start;
            element.contentStart = tag.end;
            element.selfClosing = tag.selfClosing;
            if (tag.selfClosing) {
                element.contentEnd = tag.end;
                element.end = tag.end;
            } else if (!findElementEnd(xml, tag, element.contentEnd, element.end)) {
                return false;
            }
            if (element.end > end) return false;
            found = true;
            return true;
        }
        if (!tag.selfClosing) ++depth;
    }
    return true;
}

bool isSuccessfulHttpStatus(const std::string& xml, const XmlElement& status) {
    if (status.selfClosing) return false;
    size_t begin = status.contentStart;
    size_t end = status.contentEnd;
    while (begin < end && isspace(static_cast<unsigned char>(xml[begin]))) ++begin;
    while (end > begin && isspace(static_cast<unsigned char>(xml[end - 1]))) --end;

    const std::string value = xml.substr(begin, end - begin);
    if (!startsWithIgnoreCase(value, "HTTP/")) return false;
    const size_t codeStart = value.find_first_of(" \t");
    if (codeStart == std::string::npos) return false;
    size_t codePosition = value.find_first_not_of(" \t", codeStart);
    if (codePosition == std::string::npos || codePosition + 3 > value.size()) return false;
    if (!isdigit(static_cast<unsigned char>(value[codePosition])) ||
        !isdigit(static_cast<unsigned char>(value[codePosition + 1])) ||
        !isdigit(static_cast<unsigned char>(value[codePosition + 2])) ||
        (codePosition + 3 < value.size() &&
         !isspace(static_cast<unsigned char>(value[codePosition + 3])))) {
        return false;
    }
    const int code = (value[codePosition] - '0') * 100 +
        (value[codePosition + 1] - '0') * 10 + value[codePosition + 2] - '0';
    return code >= 200 && code < 300;
}

bool validatePropstat(const std::string& xml, const XmlElement& propstat,
                      bool& containsCalendarData) {
    containsCalendarData = false;
    XmlElement calendarData;
    bool found = false;
    if (!findNextElement(xml, propstat.contentStart, propstat.contentEnd,
                         "calendar-data", calendarData, found)) {
        return false;
    }
    containsCalendarData = found;
    if (!containsCalendarData) return true;

    bool foundStatus = false;
    size_t position = propstat.contentStart;
    while (position < propstat.contentEnd) {
        XmlElement status;
        if (!findDirectChild(xml, position, propstat.contentEnd, "status", status, found)) {
            return false;
        }
        if (!found) break;
        foundStatus = true;
        if (!isSuccessfulHttpStatus(xml, status)) return false;
        position = status.end;
    }
    return foundStatus;
}

bool validateResponse(const std::string& xml, const XmlElement& response) {
    bool hasSuccessfulCalendarData = false;
    size_t position = response.contentStart;
    while (position < response.contentEnd) {
        XmlElement propstat;
        bool found = false;
        if (!findDirectChild(xml, position, response.contentEnd, "propstat",
                             propstat, found)) {
            return false;
        }
        if (!found) break;
        bool containsCalendarData = false;
        if (!validatePropstat(xml, propstat, containsCalendarData)) return false;
        hasSuccessfulCalendarData = hasSuccessfulCalendarData || containsCalendarData;
        position = propstat.end;
    }

    position = response.contentStart;
    while (position < response.contentEnd) {
        XmlElement status;
        bool found = false;
        if (!findDirectChild(xml, position, response.contentEnd, "status", status, found)) {
            return false;
        }
        if (!found) break;
        if (!isSuccessfulHttpStatus(xml, status)) return false;
        position = status.end;
    }
    return hasSuccessfulCalendarData;
}

bool validateCalendarResponseStatuses(const std::string& xml) {
    XmlElement multistatus;
    size_t position = 0;
    XmlTag root;
    bool found = false;
    if (!nextXmlTag(xml, position, root, found) || !found || root.closing ||
        root.localName != "multistatus" || root.selfClosing) {
        return false;
    }
    multistatus.name = root.name;
    multistatus.start = root.start;
    multistatus.contentStart = root.end;
    if (!findElementEnd(xml, root, multistatus.contentEnd, multistatus.end)) return false;

    position = multistatus.contentStart;
    while (position < multistatus.contentEnd) {
        XmlElement response;
        if (!findDirectChild(xml, position, multistatus.contentEnd, "response",
                             response, found)) {
            return false;
        }
        if (!found) break;
        if (!validateResponse(xml, response)) return false;
        position = response.end;
    }
    return true;
}

} // namespace

bool calendarParseResponse(const std::string& xml,
                           std::vector<CalendarEvent>& events) {
    if (!validateCalendarResponseStatuses(xml)) return false;
    size_t position = 0;
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
    return true;
}

namespace {

struct RecurrenceDay {
    int weekday = 0;
    int ordinal = 0;
};

struct RecurrenceRule {
    std::string frequency;
    std::string until;
    std::vector<int> byMonth;
    std::vector<int> byMonthDay;
    std::vector<RecurrenceDay> byDay;
    int interval = 1;
    int count = 0;
    int weekStart = 1;
    bool hasCount = false;
};

bool parseRuleInteger(const std::string& value, int minimum, int maximum, int& output) {
    if (value.empty()) return false;
    char* end = nullptr;
    const long parsed = strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < minimum || parsed > maximum) {
        return false;
    }
    output = static_cast<int>(parsed);
    return true;
}

bool parseWeekday(const std::string& value, int& weekday) {
    static const char* WEEKDAYS[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; ++i) {
        if (equalsIgnoreCase(value, WEEKDAYS[i])) {
            weekday = i;
            return true;
        }
    }
    return false;
}

bool parseIntegerList(const std::string& value, int minimum, int maximum,
                      std::vector<int>& output) {
    size_t position = 0;
    while (position < value.size()) {
        size_t end = value.find(',', position);
        if (end == std::string::npos) end = value.size();
        int parsed = 0;
        if (!parseRuleInteger(value.substr(position, end - position), minimum, maximum, parsed)) {
            return false;
        }
        output.push_back(parsed);
        position = end + 1;
    }
    return !output.empty();
}

bool parseRecurrenceRule(const char* text, RecurrenceRule& rule) {
    const std::string value(text);
    size_t position = 0;
    while (position < value.size()) {
        size_t end = value.find(';', position);
        if (end == std::string::npos) end = value.size();
        const size_t equals = value.find('=', position);
        if (equals == std::string::npos || equals >= end) return false;
        const std::string key = value.substr(position, equals - position);
        const std::string field = value.substr(equals + 1, end - equals - 1);
        if (equalsIgnoreCase(key, "FREQ")) {
            rule.frequency = field;
        } else if (equalsIgnoreCase(key, "INTERVAL")) {
            if (!parseRuleInteger(field, 1, 100000, rule.interval)) return false;
        } else if (equalsIgnoreCase(key, "COUNT")) {
            if (!parseRuleInteger(field, 1, 1000000, rule.count)) return false;
            rule.hasCount = true;
        } else if (equalsIgnoreCase(key, "UNTIL")) {
            IcsDateTime until {};
            if (!parseIcsDateTime(field, until)) return false;
            rule.until = field;
        } else if (equalsIgnoreCase(key, "BYMONTH")) {
            if (!parseIntegerList(field, 1, 12, rule.byMonth)) return false;
        } else if (equalsIgnoreCase(key, "BYMONTHDAY")) {
            if (!parseIntegerList(field, -31, 31, rule.byMonthDay)) return false;
            if (std::find(rule.byMonthDay.begin(), rule.byMonthDay.end(), 0) !=
                rule.byMonthDay.end()) return false;
        } else if (equalsIgnoreCase(key, "BYDAY")) {
            size_t itemPosition = 0;
            while (itemPosition < field.size()) {
                size_t itemEnd = field.find(',', itemPosition);
                if (itemEnd == std::string::npos) itemEnd = field.size();
                const std::string item = field.substr(itemPosition, itemEnd - itemPosition);
                size_t weekdayPosition = item.size();
                while (weekdayPosition > 0 &&
                       item[weekdayPosition - 1] >= 'A' && item[weekdayPosition - 1] <= 'Z') {
                    --weekdayPosition;
                }
                RecurrenceDay day {};
                const std::string ordinal = item.substr(0, weekdayPosition);
                if (!parseWeekday(item.substr(weekdayPosition), day.weekday) ||
                    (!ordinal.empty() && !parseRuleInteger(ordinal, -5, 5, day.ordinal)) ||
                    (!ordinal.empty() && day.ordinal == 0)) {
                    return false;
                }
                rule.byDay.push_back(day);
                itemPosition = itemEnd + 1;
            }
        } else if (equalsIgnoreCase(key, "WKST")) {
            if (!parseWeekday(field, rule.weekStart)) return false;
        } else {
            return false;
        }
        position = end + 1;
    }
    if (rule.frequency.empty() ||
        (!equalsIgnoreCase(rule.frequency, "DAILY") &&
         !equalsIgnoreCase(rule.frequency, "WEEKLY") &&
         !equalsIgnoreCase(rule.frequency, "MONTHLY") &&
         !equalsIgnoreCase(rule.frequency, "YEARLY")) ||
        (rule.hasCount && !rule.until.empty()) ||
        (equalsIgnoreCase(rule.frequency, "WEEKLY") && !rule.byMonthDay.empty()) ||
        ((equalsIgnoreCase(rule.frequency, "WEEKLY") ||
          equalsIgnoreCase(rule.frequency, "DAILY")) &&
         std::any_of(rule.byDay.begin(), rule.byDay.end(),
                     [](const RecurrenceDay& day) { return day.ordinal != 0; })) ||
        (equalsIgnoreCase(rule.frequency, "YEARLY") && rule.byMonth.empty() &&
         std::any_of(rule.byDay.begin(), rule.byDay.end(),
                     [](const RecurrenceDay& day) { return day.ordinal != 0; }))) {
        return false;
    }
    return true;
}

bool parseDateKey(const char* value, IcsDateTime& dateTime) {
    if (strlen(value) != 10 || value[4] != '-' || value[7] != '-') return false;
    char compact[9] {
        value[0], value[1], value[2], value[3],
        value[5], value[6], value[8], value[9], '\0'
    };
    return parseIcsDateTime(compact, dateTime);
}

int64_t dateSerial(const IcsDateTime& dateTime) {
    return civilSeconds(dateTime) / 86400;
}

bool dateKeyFromSerial(int64_t serial, char output[11]) {
    int64_t days = serial + 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    int year = static_cast<int>(yearOfEra) + static_cast<int>(era * 400);
    const unsigned dayOfYear =
        dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
    const unsigned day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
    const unsigned month = monthPrime < 10 ? monthPrime + 3 : monthPrime - 9;
    year += month <= 2;
    return calendarFormatDateKey(output, 11, year, static_cast<int>(month),
                                 static_cast<int>(day));
}

bool monthDayMatches(int day, int days, const std::vector<int>& values) {
    if (values.empty()) return true;
    return std::any_of(values.begin(), values.end(), [day, days](int value) {
        return value > 0 ? day == value : day == days + value + 1;
    });
}

bool recurrenceDayMatches(int year, int month, int day,
                          const std::vector<RecurrenceDay>& values) {
    if (values.empty()) return true;
    const int days = daysInMonth(year, month);
    return std::any_of(values.begin(), values.end(), [=](const RecurrenceDay& ruleDay) {
        if (weekdayForDate(year, month, day) != ruleDay.weekday) return false;
        if (ruleDay.ordinal > 0) return (day - 1) / 7 + 1 == ruleDay.ordinal;
        if (ruleDay.ordinal < 0) return (days - day) / 7 + 1 == -ruleDay.ordinal;
        return true;
    });
}

bool recurrenceMatchesDate(const IcsDateTime& start, const IcsDateTime& candidate,
                           const RecurrenceRule& rule) {
    const int64_t startSerial = dateSerial(start);
    const int64_t candidateSerial = dateSerial(candidate);
    if (candidateSerial < startSerial) return false;
    if (candidateSerial == startSerial) return true;

    if (equalsIgnoreCase(rule.frequency, "DAILY")) {
        if ((candidateSerial - startSerial) % rule.interval != 0) return false;
    } else if (equalsIgnoreCase(rule.frequency, "WEEKLY")) {
        const int startOffset = (weekdayForDate(start.year, start.month, start.day) -
                                 rule.weekStart + 7) % 7;
        const int candidateOffset = (weekdayForDate(candidate.year, candidate.month, candidate.day) -
                                     rule.weekStart + 7) % 7;
        const int64_t startWeek = startSerial - startOffset;
        const int64_t candidateWeek = candidateSerial - candidateOffset;
        if ((candidateWeek - startWeek) / 7 % rule.interval != 0) return false;
    } else if (equalsIgnoreCase(rule.frequency, "MONTHLY")) {
        const int monthDifference = (candidate.year - start.year) * 12 +
            candidate.month - start.month;
        if (monthDifference < 0 || monthDifference % rule.interval != 0) return false;
    } else {
        const int yearDifference = candidate.year - start.year;
        if (yearDifference < 0 || yearDifference % rule.interval != 0) return false;
    }

    if (!rule.byMonth.empty()) {
        if (std::find(rule.byMonth.begin(), rule.byMonth.end(), candidate.month) ==
            rule.byMonth.end()) return false;
    } else if (equalsIgnoreCase(rule.frequency, "YEARLY") &&
               rule.byMonthDay.empty() && rule.byDay.empty() &&
               candidate.month != start.month) {
        return false;
    }

    if (!monthDayMatches(candidate.day, daysInMonth(candidate.year, candidate.month),
                        rule.byMonthDay)) {
        return false;
    }
    if (!rule.byDay.empty()) {
        if (!recurrenceDayMatches(candidate.year, candidate.month, candidate.day, rule.byDay)) {
            return false;
        }
    } else if (equalsIgnoreCase(rule.frequency, "WEEKLY")) {
        const int weekday = rule.byDay.empty()
            ? weekdayForDate(start.year, start.month, start.day)
            : rule.byDay.front().weekday;
        if (weekdayForDate(candidate.year, candidate.month, candidate.day) != weekday) return false;
    } else if ((equalsIgnoreCase(rule.frequency, "MONTHLY") ||
                equalsIgnoreCase(rule.frequency, "YEARLY")) &&
               rule.byMonthDay.empty() && candidate.day != start.day) {
        return false;
    }
    return true;
}

bool recurrenceUntilAllows(const CalendarEvent& event, const RecurrenceRule& rule,
                           const IcsDateTime& candidate) {
    if (rule.until.empty()) return true;
    IcsDateTime until {};
    if (!parseIcsDateTime(rule.until, until)) return false;
    const int64_t candidateDay = dateSerial(candidate);
    const int64_t untilDay = dateSerial(until);
    if (candidateDay != untilDay) return candidateDay < untilDay;
    if (until.dateOnly) return true;
    if (event.recurrenceLocalId[0] == 'D') return true;
    if ((event.recurrenceLocalId[0] == 'U') != until.utc) return true;
    const char* time = event.recurrenceLocalId + 10;
    const int eventSeconds = (time[0] - '0') * 36000 + (time[1] - '0') * 3600 +
        (time[2] - '0') * 600 + (time[3] - '0') * 60 +
        (time[4] - '0') * 10 + time[5] - '0';
    const int untilSeconds = until.hour * 3600 + until.minute * 60 + until.second;
    return eventSeconds <= untilSeconds;
}

bool recurrenceCountAllows(const IcsDateTime& start, const IcsDateTime& candidate,
                           const RecurrenceRule& rule, bool& complete) {
    if (!rule.hasCount) return true;
    const int64_t startSerial = dateSerial(start);
    const int64_t candidateSerial = dateSerial(candidate);
    int count = 0;
    for (int64_t serial = startSerial; serial <= candidateSerial; ++serial) {
        if (serial - startSerial > 100000) {
            complete = false;
            return false;
        }
        IcsDateTime day {};
        char dateKey[11] {};
        if (!dateKeyFromSerial(serial, dateKey) || !parseDateKey(dateKey, day)) {
            complete = false;
            return false;
        }
        if (recurrenceMatchesDate(start, day, rule)) {
            if (++count > rule.count) return false;
        }
    }
    return true;
}

bool recurrenceDateTimeFromLocalId(const char* localId, IcsDateTime& dateTime) {
    if (localId[0] == 'D') return parseIcsDateTime(localId + 1, dateTime);
    if ((localId[0] != 'L' && localId[0] != 'U') || strlen(localId) != 16 ||
        localId[9] != 'T') {
        return false;
    }
    char value[17] {};
    memcpy(value, localId + 1, 8);
    value[8] = 'T';
    memcpy(value + 9, localId + 10, 6);
    return parseIcsDateTime(value, dateTime);
}

bool eventOverlapsMonth(const CalendarEvent& event,
                        const char* monthStart, const char* monthEnd) {
    return strcmp(event.startDate, monthEnd) < 0 &&
        strcmp(event.endDateExclusive, monthStart) > 0;
}

bool hasRecurrenceOverride(const std::vector<CalendarEvent>& events,
                           const CalendarEvent& master, const char* localId) {
    if (master.uid[0] == '\0') return false;
    for (const CalendarEvent& event : events) {
        if (event.recurrenceOverride && strcmp(event.uid, master.uid) == 0 &&
            strcmp(event.recurrenceLocalId, localId) == 0) {
            return true;
        }
    }
    return false;
}

bool isExcludedDate(const CalendarEvent& event, const char* localId) {
    size_t position = 0;
    const size_t length = strlen(event.excludedDates);
    while (position < length) {
        size_t end = position;
        while (end < length && event.excludedDates[end] != ',') ++end;
        if (end - position == strlen(localId) &&
            strncmp(event.excludedDates + position, localId, end - position) == 0) {
            return true;
        }
        position = end + 1;
    }
    return false;
}

void makeOccurrenceLocalId(const CalendarEvent& master, const char* date,
                           char output[20]) {
    if (master.recurrenceLocalId[0] == 'D') {
        snprintf(output, 20, "D%.4s%.2s%.2s", date, date + 5, date + 8);
        return;
    }
    copyText(output, 20, master.recurrenceLocalId);
    memcpy(output + 1, date, 4);
    memcpy(output + 5, date + 5, 2);
    memcpy(output + 7, date + 8, 2);
}

bool addRecurrenceOccurrence(const CalendarEvent& master, const char* date,
                             int64_t daySerialValue, int64_t durationDays,
                             std::vector<CalendarEvent>& output) {
    CalendarEvent occurrence = master;
    copyText(occurrence.startDate, sizeof(occurrence.startDate), date);
    if (!dateKeyFromSerial(daySerialValue + durationDays, occurrence.endDateExclusive)) {
        return false;
    }
    if (strlen(master.start) >= 10) {
        char formattedStart[sizeof(master.start)] {};
        snprintf(formattedStart, sizeof(formattedStart), "%s%s", date, master.start + 10);
        copyText(occurrence.start, sizeof(occurrence.start), formattedStart);
    }
    occurrence.recurrenceRule[0] = '\0';
    occurrence.recurrenceOverride = false;
    occurrence.cancelled = false;
    occurrence.recurrenceUnsupported = false;
    makeOccurrenceLocalId(master, date, occurrence.recurrenceLocalId);
    snprintf(occurrence.occurrenceId, sizeof(occurrence.occurrenceId),
             "R%s", occurrence.recurrenceLocalId);
    output.push_back(occurrence);
    return true;
}

bool expandRecurringEvent(const std::vector<CalendarEvent>& events,
                          const CalendarEvent& master, int64_t monthStart,
                          int64_t monthEnd, const char* monthStartText,
                          const char* monthEndText, int64_t durationDays,
                          std::vector<CalendarEvent>& output, bool& complete) {
    if (master.recurrenceUnsupported) complete = false;
    RecurrenceRule rule {};
    if (!parseRecurrenceRule(master.recurrenceRule, rule)) {
        complete = false;
        if (eventOverlapsMonth(master, monthStartText, monthEndText) &&
            !isExcludedDate(master, master.recurrenceLocalId)) {
            CalendarEvent initial = master;
            initial.recurrenceRule[0] = '\0';
            initial.recurrenceUnsupported = false;
            output.push_back(initial);
        }
        return false;
    }

    IcsDateTime start {};
    if (!recurrenceDateTimeFromLocalId(master.recurrenceLocalId, start)) {
        complete = false;
        return false;
    }
    const int64_t startDay = dateSerial(start);
    if (startDay >= monthEnd) return true;
    const int64_t lookback = std::min<int64_t>(std::max<int64_t>(durationDays, 1), 3660);
    const int64_t scanStart = std::max(startDay, monthStart - lookback);
    if (durationDays > lookback) complete = false;
    for (int64_t serial = scanStart; serial < monthEnd; ++serial) {
        char date[11] {};
        IcsDateTime candidate {};
        if (!dateKeyFromSerial(serial, date) || !parseDateKey(date, candidate)) {
            complete = false;
            return false;
        }
        if (!recurrenceMatchesDate(start, candidate, rule) ||
            !recurrenceUntilAllows(master, rule, candidate)) {
            continue;
        }
        if (!recurrenceCountAllows(start, candidate, rule, complete)) {
            if (!complete) return false;
            continue;
        }
        char localId[20] {};
        makeOccurrenceLocalId(master, date, localId);
        if (hasRecurrenceOverride(events, master, localId) ||
            isExcludedDate(master, localId)) {
            continue;
        }
        if (!addRecurrenceOccurrence(master, date, serial,
                                    std::max<int64_t>(1, durationDays), output)) {
            complete = false;
            return false;
        }
    }
    return true;
}

} // namespace

bool calendarFormatDateKey(char* output, size_t size, int year, int month, int day) {
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


bool calendarEventOverlapsDay(const CalendarEvent& event, const char* date) {
    return eventOverlapsDay(event, date);
}

bool calendarExpandEventsForMonth(const std::vector<CalendarEvent>& events,
                                  int year, int month,
                                  std::vector<CalendarEvent>& monthEvents) {
    monthEvents.clear();
    if (year < 1000 || year > 9999 || month < 1 || month > 12) return false;
    char monthStartText[11] {};
    char monthEndText[11] {};
    if (!calendarFormatDateKey(monthStartText, sizeof(monthStartText), year, month, 1)) {
        return false;
    }
    const int nextYear = month == 12 ? year + 1 : year;
    const int nextMonth = month == 12 ? 1 : month + 1;
    if (!calendarFormatDateKey(monthEndText, sizeof(monthEndText), nextYear, nextMonth, 1)) {
        return false;
    }
    IcsDateTime monthStartDate {};
    IcsDateTime monthEndDate {};
    if (!parseDateKey(monthStartText, monthStartDate) ||
        !parseDateKey(monthEndText, monthEndDate)) {
        return false;
    }
    const int64_t monthStart = dateSerial(monthStartDate);
    const int64_t monthEnd = dateSerial(monthEndDate);
    bool complete = true;

    for (const CalendarEvent& event : events) {
        if (event.cancelled) continue;
        if (event.recurrenceOverride || event.recurrenceRule[0] == '\0') {
            if (eventOverlapsMonth(event, monthStartText, monthEndText)) {
                monthEvents.push_back(event);
            }
            if (event.recurrenceUnsupported) complete = false;
            continue;
        }

        IcsDateTime eventStart {};
        IcsDateTime eventEnd {};
        if (!parseDateKey(event.startDate, eventStart) ||
            !parseDateKey(event.endDateExclusive, eventEnd)) {
            complete = false;
            continue;
        }
        const int64_t durationDays = dateSerial(eventEnd) - dateSerial(eventStart);
        if (durationDays <= 0) {
            complete = false;
            continue;
        }
        expandRecurringEvent(events, event, monthStart, monthEnd,
                             monthStartText, monthEndText, durationDays,
                             monthEvents, complete);
    }
    std::sort(monthEvents.begin(), monthEvents.end(), [](const CalendarEvent& left,
                                                         const CalendarEvent& right) {
        const int startCompare = strcmp(left.start, right.start);
        if (startCompare != 0) return startCompare < 0;
        const int uidCompare = strcmp(left.uid, right.uid);
        return uidCompare == 0 ? strcmp(left.summary, right.summary) < 0 : uidCompare < 0;
    });
    return complete;
}
