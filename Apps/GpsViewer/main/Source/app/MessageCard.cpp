// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tabs.h"
#include "Widgets.h"

#include <lvgl/devices/keyboard.h>

#include <cstdio>

namespace gpsviewer {

namespace {

bool hasMessage(SourceStatus status) {
    switch (status) {
        case SourceStatus::NoReceiver:
        case SourceStatus::LocationOff:
        case SourceStatus::ReceiverStarting:
        case SourceStatus::ReceiverLost:
            return true;
        default:
            return false;
    }
}

bool hasSettingsButton(SourceStatus status) {
    return status == SourceStatus::NoReceiver || status == SourceStatus::LocationOff ||
           status == SourceStatus::ReceiverStarting;
}

} // namespace

void MessageCard::create(lv_obj_t* parent, const Theme& theme, lv_event_cb_t onSettings, lv_event_cb_t onKey, void* userData) {
    card = createSection(parent);
    lv_obj_set_width(card, LV_PCT(92));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_pad_all(card, LV_DPX(8), LV_PART_MAIN);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(card, LV_DPX(5), LV_PART_MAIN);

    createIcon(card, IconKind::Alert, LV_DPX(28), theme.warning, theme.background);
    titleLabel = createLabel(card, FONT_SIZE_LARGE, theme.text);
    lv_obj_set_width(titleLabel, LV_PCT(100));
    lv_obj_set_style_text_align(titleLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    helpLabel = createLabel(card, FONT_SIZE_SMALL, theme.dim);
    lv_label_set_long_mode(helpLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(helpLabel, LV_PCT(100));
    lv_obj_set_style_text_align(helpLabel, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    // The theme styles the button, except in night mode
    button = lv_button_create(card);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, "Open settings");
    if (theme.night) {
        lv_obj_set_style_bg_color(button, theme.accent, LV_PART_MAIN);
        lv_obj_set_style_text_color(label, theme.background, LV_PART_MAIN);
        lv_obj_set_style_outline_color(button, theme.text, LV_STATE_FOCUS_KEY);
    }
    lv_obj_add_event_cb(button, onSettings, LV_EVENT_CLICKED, userData);
    lv_obj_add_event_cb(button, onKey, LV_EVENT_KEY, userData);
    lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    visible = false;
    buttonInGroup = lv_group_get_default() != nullptr;
    // Only in the group while there is a button to use
    if (buttonInGroup) {
        lv_group_remove_obj(button);
        buttonInGroup = false;
    }
}

void MessageCard::clear() {
    card = nullptr;
    titleLabel = nullptr;
    helpLabel = nullptr;
    button = nullptr;
    visible = false;
    buttonInGroup = false;
}

bool MessageCard::update(SourceStatus status, const char* dataDirectory) {
    if (card == nullptr) {
        return false;
    }
    const bool show = hasMessage(status);
    if (!show) {
        if (buttonInGroup && button != nullptr) {
            lv_group_remove_obj(button);
            buttonInGroup = false;
        }
        setHidden(card, true);
        visible = false;
        return false;
    }
    char help[PATH_LENGTH + 40];
    const char* title = "";
    help[0] = '\0';
    switch (status) {
        case SourceStatus::NoReceiver:
            title = "No GPS receiver";
            snprintf(help, sizeof(help), "Set one up in Tactility's GPS settings.");
            break;
        case SourceStatus::LocationOff:
            title = "Location is off";
            snprintf(help, sizeof(help), "Turn it on in Tactility's location settings.");
            break;
        case SourceStatus::ReceiverStarting:
            title = "Starting GPS receiver...";
            snprintf(help, sizeof(help), "If this takes long, check the receiver in the GPS settings.");
            break;
        case SourceStatus::ReceiverLost:
            title = "GPS receiver stopped";
            snprintf(help, sizeof(help), "Waiting for it to come back...");
            break;
        default:
            break;
    }
    setLabelText(titleLabel, title);
    setLabelText(helpLabel, help);

    const bool withButton = hasSettingsButton(status);
    setHidden(button, !withButton);
    // The app's group, also while a menu overlay has its own group
    lv_group_t* group = lvgl_keyboard_get_app_group();
    if (button != nullptr && group != nullptr) {
        if (withButton && !buttonInGroup) {
            lv_group_add_obj(group, button);
            buttonInGroup = true;
        } else if (!withButton && buttonInGroup) {
            lv_group_remove_obj(button);
            buttonInGroup = false;
        }
    }
    setHidden(card, false);
    visible = true;
    return true;
}

} // namespace gpsviewer
