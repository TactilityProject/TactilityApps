// SPDX-License-Identifier: GPL-3.0-or-later
#include "ClockUi.h"

#include <app/event.h>
#include <app/start.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/shared.h>
#include <lvgl/insets.h>
#include <lvgl/theme.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/error.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace clockapp {
namespace {

constexpr const char* WIFI_MANAGER_APP_ID = "tactility.wifimanage";
constexpr int32_t MIN_VALID_YEAR = 2025;

struct ContentMetrics {
    lv_coord_t width;
    lv_coord_t height;
    bool compact;
};

void rebuildContent(Context* context);

int32_t clampInt(int32_t value, int32_t minimum, int32_t maximum) {
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

lv_coord_t trigOffset(lv_coord_t radius, int32_t angle, bool sine) {
    int16_t angle_value = static_cast<int16_t>(angle);
    int32_t factor = sine ? lv_trigo_sin(angle_value) : lv_trigo_cos(angle_value);
    return static_cast<lv_coord_t>((static_cast<int32_t>(radius) * factor) / 32768);
}

lv_coord_t themePadding() {
    return static_cast<lv_coord_t>(lvgl_theme_lerp(8, 20, lvgl_theme_get_spacing()));
}

lv_coord_t themeGap() {
    return static_cast<lv_coord_t>(lvgl_theme_lerp(4, 10, lvgl_theme_get_spacing()));
}

lv_color_t accentColor(lv_obj_t* reference) {
    if (!lvgl_theme_is_mono()) {
        LvglThemeSettings settings {};
        lvgl_theme_get_settings(&settings);
        return settings.color_primary;
    }
    return lv_obj_get_style_text_color(reference, LV_PART_MAIN);
}

bool isTimeValid(const tm& local_time) {
    return local_time.tm_year + 1900 >= MIN_VALID_YEAR;
}

ContentMetrics getContentMetrics(const Context* context) {
    ContentMetrics metrics {};
    metrics.width = lv_obj_get_content_width(context->content);
    metrics.height = lv_obj_get_content_height(context->content);
    metrics.compact = metrics.width < 320 || metrics.height < 180;
    return metrics;
}

lv_obj_t* createWrapper(lv_obj_t* parent, int32_t width, lv_coord_t gap, lv_flex_flow_t flow) {
    lv_obj_t* wrapper = lv_obj_create(parent);
    lv_obj_remove_style_all(wrapper);
    lv_obj_set_size(wrapper, width, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(wrapper, flow);
    lv_obj_set_flex_align(
        wrapper,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_set_style_pad_all(wrapper, 0, 0);
    lv_obj_set_style_pad_gap(wrapper, gap, 0);
    lv_obj_set_scrollable(wrapper, false);
    return wrapper;
}

lv_obj_t* createColumn(lv_obj_t* parent, int32_t width, lv_coord_t gap) {
    return createWrapper(parent, width, gap, LV_FLEX_FLOW_COLUMN);
}

lv_obj_t* createRow(lv_obj_t* parent, int32_t width, lv_coord_t gap) {
    return createWrapper(parent, width, gap, LV_FLEX_FLOW_ROW);
}

void onClose(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(context->appInstanceId);
}

void onModeToggle(lv_event_t* event) {
    auto* context = static_cast<Context*>(lv_event_get_user_data(event));
    if (!context->timeValid) {
        return;
    }

    context->analog = !context->analog;
    saveAnalogMode(context->analog);
    rebuildContent(context);
    refreshClock(context);
}

void onWifiConnect(lv_event_t* event) {
    (void)event;

    AppStartContext start_context {};
    if (app_start_context_from_id(WIFI_MANAGER_APP_ID, &start_context) != ERROR_NONE) {
        return;
    }

    AppInstanceId instance_id = 0;
    app_start_with_context(&start_context, &instance_id);
}

void updateModeButton(Context* context) {
    if (context->modeButton == nullptr) {
        return;
    }

    if (context->timeValid) {
        lv_obj_set_hidden(context->modeButton, false);
    } else {
        lv_obj_set_hidden(context->modeButton, true);
    }

    if (context->modeButtonLabel != nullptr) {
        lv_label_set_text(context->modeButtonLabel, context->analog ? "Digital" : "Analog");
    }
}

void resetContentPointers(Context* context) {
    context->timeLabel = nullptr;
    context->secondsLabel = nullptr;
    context->meridiemLabel = nullptr;
    context->weekdayLabel = nullptr;
    context->dateLabel = nullptr;
    context->invalidIcon = nullptr;
    context->invalidBody = nullptr;
    context->analogFace = nullptr;
    context->hourHand = nullptr;
    context->minuteHand = nullptr;
    context->secondHand = nullptr;
    context->centerCap = nullptr;
    for (lv_obj_t*& line : context->tickLines) {
        line = nullptr;
    }
}

void updateInvalidLayout(Context* context) {
    ContentMetrics metrics = getContentMetrics(context);
    bool compact = metrics.height < 160 || metrics.width < 280;

    if (context->invalidIcon != nullptr) {
        if (compact) {
            lv_obj_set_hidden(context->invalidIcon, true);
        } else {
            lv_obj_set_hidden(context->invalidIcon, false);
        }
    }
    if (context->invalidBody != nullptr) {
        if (compact) {
            lv_obj_set_hidden(context->invalidBody, true);
        } else {
            lv_obj_set_hidden(context->invalidBody, false);
        }
    }

    if (context->invalidIcon != nullptr) {
        lv_obj_t* card = lv_obj_get_parent(context->invalidIcon);
        lv_obj_set_style_pad_all(card, compact ? 6 : themePadding(), 0);
        lv_obj_set_style_pad_gap(card, compact ? 2 : themeGap(), 0);
    }
}

void buildInvalidContent(Context* context) {
    lv_obj_t* card = lvgl_card_create(context->content);
    lv_obj_set_width(card, LV_PCT(90));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(card, 480, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        card,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_set_style_pad_all(card, themePadding(), 0);
    lv_obj_set_style_pad_gap(card, themeGap(), 0);
    lv_obj_set_scrollable(card, false);

    context->invalidIcon = lv_label_create(card);
    lv_obj_set_style_text_font(context->invalidIcon, lvgl_get_shared_icon_large_font(), 0);
    lv_obj_set_style_text_color(context->invalidIcon, accentColor(context->content), 0);
    lv_label_set_text(context->invalidIcon, LVGL_ICON_SHARED_WIFI);

    lv_obj_t* title = lv_label_create(card);
    lv_obj_set_width(title, LV_PCT(100));
    lv_obj_set_style_text_font(title, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(title, "Time not set");
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_CLIP);

    context->invalidBody = lv_label_create(card);
    lv_obj_set_width(context->invalidBody, LV_PCT(100));
    lv_obj_set_style_text_font(context->invalidBody, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(context->invalidBody, LV_OPA_70, 0);
    lv_obj_set_style_text_align(context->invalidBody, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(context->invalidBody, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(context->invalidBody, "Connect to Wi-Fi to sync the clock automatically.");

    lv_obj_t* button = lv_button_create(card);
    lv_obj_set_size(button, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_add_event_cb(button, onWifiConnect, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Connect to Wi-Fi");
    lv_obj_center(button_label);
}

void formatDate(const Context* context, const tm& local_time, char* output, size_t output_size) {
    if (strftime(output, output_size, dateFormatPattern(context->dateFormat), &local_time) == 0) {
        output[0] = '\0';
    }
}

void updateDigital(Context* context, const tm& local_time) {
    ContentMetrics metrics = getContentMetrics(context);
    bool compact = metrics.compact;
    bool show_date = metrics.height >= 60;

    char time_text[16];
    if (context->format24h) {
        snprintf(time_text, sizeof(time_text), "%02d:%02d", local_time.tm_hour, local_time.tm_min);
    } else {
        int hour = local_time.tm_hour % 12;
        if (hour == 0) {
            hour = 12;
        }
        snprintf(time_text, sizeof(time_text), "%d:%02d", hour, local_time.tm_min);
    }

    if (context->timeLabel != nullptr) {
        const lv_font_t* font = metrics.width < 200
            ? lvgl_get_text_font(FONT_SIZE_DEFAULT)
            : lvgl_get_text_font(FONT_SIZE_LARGE);
        lv_obj_set_style_text_font(context->timeLabel, font, 0);
        lv_label_set_text(context->timeLabel, time_text);
    }

    if (context->secondsLabel != nullptr) {
        char seconds_text[8];
        snprintf(seconds_text, sizeof(seconds_text), ":%02d", local_time.tm_sec);
        lv_label_set_text(context->secondsLabel, seconds_text);
    }

    if (context->meridiemLabel != nullptr) {
        if (!context->format24h) {
            char meridiem[8];
            if (strftime(meridiem, sizeof(meridiem), "%p", &local_time) > 0) {
                lv_label_set_text(context->meridiemLabel, meridiem);
                lv_obj_set_hidden(context->meridiemLabel, false);
            } else {
                lv_obj_set_hidden(context->meridiemLabel, true);
            }
        } else {
            lv_obj_set_hidden(context->meridiemLabel, true);
        }
    }

    char date_text[48];
    if (compact) {
        char numeric_date[32];
        char weekday[16];
        formatDate(context, local_time, numeric_date, sizeof(numeric_date));
        if (strftime(weekday, sizeof(weekday), "%a", &local_time) == 0) {
            weekday[0] = '\0';
        }
        snprintf(date_text, sizeof(date_text), "%s %s", weekday, numeric_date);

        if (context->weekdayLabel != nullptr) {
            lv_obj_set_hidden(context->weekdayLabel, true);
        }
    } else {
        formatDate(context, local_time, date_text, sizeof(date_text));

        if (context->weekdayLabel != nullptr) {
            char weekday[16];
            if (strftime(weekday, sizeof(weekday), "%A", &local_time) > 0) {
                lv_label_set_text(context->weekdayLabel, weekday);
                lv_obj_set_hidden(context->weekdayLabel, false);
            } else {
                lv_obj_set_hidden(context->weekdayLabel, true);
            }
        }
    }

    if (context->dateLabel != nullptr) {
        if (show_date) {
            lv_label_set_text(context->dateLabel, date_text);
            lv_obj_set_hidden(context->dateLabel, false);
        } else {
            lv_obj_set_hidden(context->dateLabel, true);
        }
    }
}

void buildDigitalContent(Context* context) {
    lv_obj_t* root = createColumn(context->content, LV_PCT(100), themeGap());

    lv_obj_t* time_row = createRow(root, LV_SIZE_CONTENT, themeGap() / 2);

    context->timeLabel = lv_label_create(time_row);
    lv_obj_set_style_text_font(context->timeLabel, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    lv_obj_set_style_text_align(context->timeLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(context->timeLabel, "--:--");

    lv_obj_t* suffix_column = createColumn(time_row, LV_SIZE_CONTENT, 0);

    context->secondsLabel = lv_label_create(suffix_column);
    lv_obj_set_style_text_font(context->secondsLabel, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_obj_set_style_text_color(context->secondsLabel, accentColor(context->content), 0);
    lv_label_set_text(context->secondsLabel, ":--");

    context->meridiemLabel = lv_label_create(suffix_column);
    lv_obj_set_style_text_font(context->meridiemLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(context->meridiemLabel, LV_OPA_70, 0);
    lv_label_set_text(context->meridiemLabel, "");
    lv_obj_set_hidden(context->meridiemLabel, true);

    context->weekdayLabel = lv_label_create(root);
    lv_obj_set_width(context->weekdayLabel, LV_PCT(100));
    lv_obj_set_style_text_font(context->weekdayLabel, lvgl_get_text_font(FONT_SIZE_DEFAULT), 0);
    lv_obj_set_style_text_opa(context->weekdayLabel, LV_OPA_70, 0);
    lv_obj_set_style_text_align(context->weekdayLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(context->weekdayLabel, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(context->weekdayLabel, "");

    context->dateLabel = lv_label_create(root);
    lv_obj_set_width(context->dateLabel, LV_PCT(100));
    lv_obj_set_style_text_font(context->dateLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(context->dateLabel, LV_OPA_70, 0);
    lv_obj_set_style_text_align(context->dateLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(context->dateLabel, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(context->dateLabel, "");
}

void setHand(
    lv_obj_t* line,
    lv_point_precise_t* points,
    lv_coord_t center,
    lv_coord_t length,
    int32_t angle
) {
    points[0].x = center;
    points[0].y = center;
    points[1].x = center + trigOffset(length, angle, true);
    points[1].y = center - trigOffset(length, angle, false);
    lv_line_set_points(line, points, 2);
}

void layoutAnalog(Context* context) {
    if (context->analogFace == nullptr) {
        return;
    }

    ContentMetrics metrics = getContentMetrics(context);
    lv_coord_t available = LV_MIN(metrics.width, metrics.height);
    lv_coord_t diameter = (available * 94) / 100;
    if (diameter < 28) {
        diameter = 28;
    }

    lv_obj_set_size(context->analogFace, diameter, diameter);

    lv_color_t text_color = lv_obj_get_style_text_color(context->analogFace, LV_PART_MAIN);
    lv_color_t accent = accentColor(context->content);
    lv_coord_t center = diameter / 2;
    lv_coord_t outer_radius = center - clampInt(diameter / 40, 2, 8);
    lv_coord_t major_length = clampInt(diameter / 10, 4, 28);
    lv_coord_t minor_length = clampInt(diameter / 18, 2, 16);
    bool show_minor_ticks = diameter >= 90;

    for (int i = 0; i < 12; ++i) {
        bool major = (i % 3) == 0;
        lv_coord_t inner_radius = outer_radius - (major ? major_length : minor_length);
        int32_t angle = i * 30;

        context->tickPoints[i][0].x = center + trigOffset(inner_radius, angle, true);
        context->tickPoints[i][0].y = center - trigOffset(inner_radius, angle, false);
        context->tickPoints[i][1].x = center + trigOffset(outer_radius, angle, true);
        context->tickPoints[i][1].y = center - trigOffset(outer_radius, angle, false);

        lv_line_set_points(context->tickLines[i], context->tickPoints[i], 2);
        lv_obj_set_style_line_color(context->tickLines[i], text_color, 0);
        lv_obj_set_style_line_width(
            context->tickLines[i],
            major ? clampInt(diameter / 120, 2, 4) : 1,
            0
        );
        lv_obj_set_style_line_opa(
            context->tickLines[i],
            major ? LV_OPA_70 : LV_OPA_40,
            0
        );

        if (!major && !show_minor_ticks) {
            lv_obj_set_hidden(context->tickLines[i], true);
        } else {
            lv_obj_set_hidden(context->tickLines[i], false);
        }
    }

    lv_obj_set_style_line_width(context->hourHand, clampInt(diameter / 50, 3, 8), 0);
    lv_obj_set_style_line_width(context->minuteHand, clampInt(diameter / 65, 2, 6), 0);
    lv_obj_set_style_line_width(context->secondHand, clampInt(diameter / 120, 1, 3), 0);
    lv_obj_set_style_line_color(context->hourHand, text_color, 0);
    lv_obj_set_style_line_color(context->minuteHand, text_color, 0);
    lv_obj_set_style_line_color(context->secondHand, accent, 0);

    lv_coord_t cap_size = clampInt(diameter / 30, 3, 12);
    lv_obj_set_size(context->centerCap, cap_size, cap_size);
    lv_obj_set_style_bg_color(context->centerCap, accent, 0);
    lv_obj_center(context->centerCap);

    if (context->dateLabel != nullptr) {
        if (diameter >= 130) {
            lv_obj_set_style_text_font(context->dateLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
            lv_obj_align(
                context->dateLabel,
                LV_ALIGN_BOTTOM_MID,
                0,
                -clampInt(diameter / 12, 10, 48)
            );
            lv_obj_set_hidden(context->dateLabel, false);
        } else {
            lv_obj_set_hidden(context->dateLabel, true);
        }
    }
}

void updateAnalog(Context* context, const tm& local_time) {
    layoutAnalog(context);

    if (context->analogFace == nullptr) {
        return;
    }

    lv_coord_t size = lv_obj_get_width(context->analogFace);
    lv_coord_t center = size / 2;
    lv_coord_t hour_length = static_cast<lv_coord_t>(size * 26 / 100);
    lv_coord_t minute_length = static_cast<lv_coord_t>(size * 35 / 100);
    lv_coord_t second_length = static_cast<lv_coord_t>(size * 41 / 100);

    int32_t hour_angle = (local_time.tm_hour % 12) * 30 + local_time.tm_min / 2;
    int32_t minute_angle = local_time.tm_min * 6 + local_time.tm_sec / 10;
    int32_t second_angle = local_time.tm_sec * 6;

    setHand(context->hourHand, context->hourPoints, center, hour_length, hour_angle);
    setHand(context->minuteHand, context->minutePoints, center, minute_length, minute_angle);
    setHand(context->secondHand, context->secondPoints, center, second_length, second_angle);

    if (context->dateLabel != nullptr &&
        !lv_obj_is_hidden(context->dateLabel)) {
        char date_text[32];
        formatDate(context, local_time, date_text, sizeof(date_text));
        lv_label_set_text(context->dateLabel, date_text);
    }
}

void buildAnalogContent(Context* context) {
    lv_obj_t* face = lvgl_card_create(context->content);
    context->analogFace = face;
    lv_obj_set_size(face, 100, 100);
    lv_obj_set_style_radius(face, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(face, 0, 0);
    lv_obj_set_scrollable(face, false);

    lv_color_t text_color = lv_obj_get_style_text_color(face, LV_PART_MAIN);
    lv_color_t accent = accentColor(context->content);

    context->dateLabel = lv_label_create(face);
    lv_obj_set_style_text_font(context->dateLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(context->dateLabel, LV_OPA_70, 0);
    lv_label_set_text(context->dateLabel, "");
    lv_obj_set_hidden(context->dateLabel, true);

    for (int i = 0; i < 12; ++i) {
        context->tickLines[i] = lv_line_create(face);
        lv_obj_set_style_line_color(context->tickLines[i], text_color, 0);
        lv_obj_set_style_line_rounded(context->tickLines[i], true, 0);
    }

    context->hourHand = lv_line_create(face);
    context->minuteHand = lv_line_create(face);
    context->secondHand = lv_line_create(face);

    lv_obj_set_style_line_rounded(context->hourHand, true, 0);
    lv_obj_set_style_line_rounded(context->minuteHand, true, 0);
    lv_obj_set_style_line_rounded(context->secondHand, true, 0);
    lv_obj_set_style_line_opa(context->hourHand, LV_OPA_COVER, 0);
    lv_obj_set_style_line_opa(context->minuteHand, LV_OPA_COVER, 0);
    lv_obj_set_style_line_opa(context->secondHand, LV_OPA_COVER, 0);
    lv_obj_set_style_line_color(context->hourHand, text_color, 0);
    lv_obj_set_style_line_color(context->minuteHand, text_color, 0);
    lv_obj_set_style_line_color(context->secondHand, accent, 0);

    context->centerCap = lv_obj_create(face);
    lv_obj_set_style_border_width(context->centerCap, 0, 0);
    lv_obj_set_style_pad_all(context->centerCap, 0, 0);
    lv_obj_set_style_radius(context->centerCap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(context->centerCap, accent, 0);
    lv_obj_set_style_bg_opa(context->centerCap, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(context->centerCap, false);
}

void rebuildContent(Context* context) {
    if (context->content == nullptr) {
        return;
    }

    lv_obj_clean(context->content);
    resetContentPointers(context);
    context->contentBuilt = true;

    if (!context->timeValid) {
        buildInvalidContent(context);
    } else if (context->analog) {
        buildAnalogContent(context);
    } else {
        buildDigitalContent(context);
    }

    updateModeButton(context);
}

}

void buildUi(Context* context, lv_obj_t* parent) {
    SystemDisplaySettings settings = loadSystemDisplaySettings();
    context->format24h = settings.format24h;
    context->dateFormat = settings.dateFormat;

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        parent,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START
    );
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);
    lv_obj_set_scrollable(parent, false);

    context->toolbar = lvgl_toolbar_create(parent, "Clock");
    lvgl_toolbar_set_nav_action(context->toolbar, LV_SYMBOL_CLOSE, onClose, context);

    context->modeButton = lvgl_toolbar_add_text_button_action(
        context->toolbar,
        "Analog",
        onModeToggle,
        context
    );
    context->modeButtonLabel = lv_obj_get_child(context->modeButton, 0);
    updateModeButton(context);

    context->content = lv_obj_create(parent);
    lv_obj_set_width(context->content, LV_PCT(100));
    lv_obj_set_flex_grow(context->content, 1);
    lv_obj_set_flex_flow(context->content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        context->content,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER
    );
    lv_obj_set_style_border_width(context->content, 0, 0);
    lv_obj_set_style_bg_opa(context->content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(context->content, themePadding(), 0);
    lvgl_obj_add_edge_padding(context->content);
    lv_obj_set_scrollable(context->content, false);

    lv_obj_update_layout(parent);
    refreshClock(context);
}

void destroyUi(Context* context) {
    context->toolbar = nullptr;
    context->content = nullptr;
    context->modeButton = nullptr;
    context->modeButtonLabel = nullptr;
    resetContentPointers(context);
    context->contentBuilt = false;
    context->timeValid = false;
}

void refreshClock(Context* context) {
    if (context->content == nullptr) {
        return;
    }

    time_t now = time(nullptr);
    tm local_time {};
    localtime_r(&now, &local_time);

    bool valid = isTimeValid(local_time);
    if (!context->contentBuilt || valid != context->timeValid) {
        context->timeValid = valid;
        rebuildContent(context);
    }

    if (!valid) {
        updateInvalidLayout(context);
        return;
    }

    if (context->analog) {
        updateAnalog(context, local_time);
    } else {
        updateDigital(context, local_time);
    }
}

}
