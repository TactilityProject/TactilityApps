// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace clockapp {

enum class DateFormat {
    MonthDayYear,
    DayMonthYear,
    YearMonthDayDash,
    YearMonthDaySlash,
};

struct SystemDisplaySettings {
    bool format24h = true;
    DateFormat dateFormat = DateFormat::MonthDayYear;
};

SystemDisplaySettings loadSystemDisplaySettings();
const char* dateFormatPattern(DateFormat format);
bool loadAnalogMode();
void saveAnalogMode(bool analog);

}
