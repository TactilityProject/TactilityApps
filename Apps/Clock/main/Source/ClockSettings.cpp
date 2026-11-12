// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClockSettings.h"

#include <app/paths.h>
#include <tactility/error.h>
#include <tactility/filesystem/file.h>
#include <tactility/paths.h>
#include <tactility/preferences.h>
#include <tactility/properties_file.h>

#include <cstddef>
#include <cstdio>
#include <cstring>

namespace clockapp {
namespace {

constexpr const char* APP_ID = "tactility.clock";
constexpr const char* MODE_FILE = "clock.properties";
constexpr const char* MODE_KEY = "analog";
constexpr const char* SYSTEM_PROPERTIES_SUFFIX = "/settings/system.properties";
constexpr const char* TIME_FORMAT_KEY = "timeFormat24h";
constexpr const char* DATE_FORMAT_KEY = "dateFormat";

void buildSystemPropertiesPath(char* out_path, size_t out_path_size) {
    out_path[0] = '\0';

    char data_path[FILE_MAX_PATH_STRING_LENGTH];
    if (paths_get_data_path(data_path, sizeof(data_path)) != ERROR_NONE) {
        return;
    }

    int length = snprintf(
        out_path,
        out_path_size,
        "%s%s",
        data_path,
        SYSTEM_PROPERTIES_SUFFIX
    );
    if (length < 0 || static_cast<size_t>(length) >= out_path_size) {
        out_path[0] = '\0';
    }
}

DateFormat parseDateFormat(const char* value) {
    if (strcmp(value, "DD/MM/YYYY") == 0) {
        return DateFormat::DayMonthYear;
    }
    if (strcmp(value, "YYYY-MM-DD") == 0) {
        return DateFormat::YearMonthDayDash;
    }
    if (strcmp(value, "YYYY/MM/DD") == 0) {
        return DateFormat::YearMonthDaySlash;
    }
    return DateFormat::MonthDayYear;
}

bool openModePreferences(Preferences** out_preferences) {
    char path[FILE_MAX_PATH_STRING_LENGTH];
    if (app_paths_get_user_data_path(APP_ID, MODE_FILE, path, sizeof(path)) != ERROR_NONE) {
        return false;
    }

    Preferences* preferences = preferences_open(path);
    if (preferences == nullptr) {
        return false;
    }

    *out_preferences = preferences;
    return true;
}

}

SystemDisplaySettings loadSystemDisplaySettings() {
    SystemDisplaySettings settings;

    char path[FILE_MAX_PATH_STRING_LENGTH];
    buildSystemPropertiesPath(path, sizeof(path));
    if (path[0] == '\0' || !file_exists(path)) {
        return settings;
    }

    PropertiesFile* file = properties_file_open(path);
    if (file == nullptr) {
        return settings;
    }

    char value[16];
    if (properties_file_get(file, TIME_FORMAT_KEY, value, sizeof(value)) == ERROR_NONE) {
        settings.format24h = strcmp(value, "true") == 0;
    }
    if (properties_file_get(file, DATE_FORMAT_KEY, value, sizeof(value)) == ERROR_NONE) {
        settings.dateFormat = parseDateFormat(value);
    }

    properties_file_close(file);
    return settings;
}

const char* dateFormatPattern(DateFormat format) {
    switch (format) {
        case DateFormat::DayMonthYear:
            return "%d/%m/%Y";
        case DateFormat::YearMonthDayDash:
            return "%Y-%m-%d";
        case DateFormat::YearMonthDaySlash:
            return "%Y/%m/%d";
        case DateFormat::MonthDayYear:
        default:
            return "%m/%d/%Y";
    }
}

bool loadAnalogMode() {
    Preferences* preferences = nullptr;
    if (!openModePreferences(&preferences)) {
        return false;
    }

    bool analog = false;
    preferences_opt_bool(preferences, MODE_KEY, &analog);
    preferences_close(preferences);
    return analog;
}

void saveAnalogMode(bool analog) {
    Preferences* preferences = nullptr;
    if (!openModePreferences(&preferences)) {
        return;
    }

    preferences_put_bool(preferences, MODE_KEY, analog);
    preferences_close(preferences);
}

}
