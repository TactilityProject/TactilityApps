// SPDX-License-Identifier: GPL-3.0-or-later
#include "Tabs.h"
#include "Widgets.h"

#include <cmath>
#include <cstdio>

namespace gpsviewer {

namespace {

constexpr int DOT_SIZE = 14;

const char* dopQualityWord(float value) {
    if (std::isnan(value)) {
        return "";
    }
    if (value < 1.0f) {
        return "Ideal";
    }
    if (value < 2.0f) {
        return "Excellent";
    }
    if (value < 5.0f) {
        return "Good";
    }
    if (value < 10.0f) {
        return "Moderate";
    }
    if (value < 20.0f) {
        return "Fair";
    }
    return "Poor";
}

lv_obj_t* createRow(lv_obj_t* parent) {
    lv_obj_t* row = createBox(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, LV_DPX(4), LV_PART_MAIN);
    return row;
}

lv_obj_t* createSpacer(lv_obj_t* parent) {
    lv_obj_t* spacer = createBox(parent);
    lv_obj_set_size(spacer, 1, 1);
    lv_obj_set_flex_grow(spacer, 1);
    return spacer;
}

// Full-width block, with a divider below unless it is the last one
lv_obj_t* createBlock(lv_obj_t* parent, const Theme& theme, bool divider) {
    lv_obj_t* block = createSection(parent);
    lv_obj_set_size(block, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(block, LV_DPX(5), LV_PART_MAIN);
    if (divider) {
        setDivider(block, theme, LV_BORDER_SIDE_BOTTOM);
    }
    return block;
}

} // namespace

void StatusTab::create(lv_obj_t* parent, const Theme& theme) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(parent, 0, LV_PART_MAIN);

    // Fix card: status indicator on the left, accuracy on the right
    lv_obj_t* fixCard = createBlock(parent, theme, true);
    lv_obj_set_flex_flow(fixCard, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(fixCard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(fixCard, LV_DPX(6), LV_PART_MAIN);
    fixDot = createBox(fixCard);
    lv_obj_set_size(fixDot, DOT_SIZE, DOT_SIZE);
    lv_obj_set_style_radius(fixDot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_color(fixDot, theme.dim, LV_PART_MAIN);
    lv_obj_set_style_border_width(fixDot, 2, LV_PART_MAIN);
    fixLabel = createLabel(fixCard, FONT_SIZE_LARGE, theme.dim);
    createSpacer(fixCard);
    lv_obj_t* accuracy = createBox(fixCard);
    lv_obj_set_size(accuracy, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    // One line: "est." next to the value, so the tab fits the T-Deck's content height
    lv_obj_set_flex_flow(accuracy, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(accuracy, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(accuracy, LV_DPX(3), LV_PART_MAIN);
    lv_obj_t* estimate = createLabel(accuracy, FONT_SIZE_SMALL, theme.dim);
    lv_label_set_text(estimate, "est.");
    accuracyLabel = createLabel(accuracy, FONT_SIZE_LARGE, theme.accent);

    // DOP card: three columns
    lv_obj_t* dopCard = createBlock(parent, theme, true);
    lv_obj_set_flex_flow(dopCard, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dopCard, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    const char captions[3][5] = {"HDOP", "VDOP", "PDOP"};
    for (int i = 0; i < 3; i++) {
        lv_obj_t* column = createBox(dopCard);
        lv_obj_set_size(column, LV_PCT(32), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
        createCaption(column, theme, captions[i]);
        lv_obj_t* row = createRow(column);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        dopValue[i] = createLabel(row, FONT_SIZE_DEFAULT, theme.text);
        dopQuality[i] = createLabel(row, FONT_SIZE_SMALL, theme.dim);
    }

    // Satellites, time to first fix, receiver
    lv_obj_t* infoCard = createBlock(parent, theme, false);
    lv_obj_set_flex_flow(infoCard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(infoCard, LV_DPX(2), LV_PART_MAIN);
    lv_obj_t* satellitesRow = createRow(infoCard);
    lv_obj_t* satellitesCaption = createLabel(satellitesRow, FONT_SIZE_SMALL, theme.dim);
    lv_label_set_text(satellitesCaption, "Satellites used / in view");
    createSpacer(satellitesRow);
    satellitesLabel = createLabel(satellitesRow, FONT_SIZE_DEFAULT, theme.text);
    lv_obj_t* ttffRow = createRow(infoCard);
    lv_obj_t* ttffCaption = createLabel(ttffRow, FONT_SIZE_SMALL, theme.dim);
    lv_label_set_text(ttffCaption, "Time to first fix");
    createSpacer(ttffRow);
    ttffLabel = createLabel(ttffRow, FONT_SIZE_DEFAULT, theme.text);
    ttffNote = createLabel(ttffRow, FONT_SIZE_SMALL, theme.dim);
    receiverLabel = createLabel(infoCard, FONT_SIZE_SMALL, theme.dim);
    lv_obj_set_width(receiverLabel, LV_PCT(100));
    lastFix = -1;
}

void StatusTab::update(const TabView& view) {
    if (fixDot == nullptr || fixLabel == nullptr) {
        return;
    }
    const GnssSnapshot& s = view.snapshot;
    const Theme& theme = view.theme;
    const int fix = static_cast<int>(s.fix);
    if (fix != lastFix) {
        lastFix = fix;
        const lv_color_t color = fixColor(s.fix, theme);
        // Filled dot with a fix, hollow ring without
        lv_obj_set_style_bg_color(fixDot, color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(fixDot, s.fix == FixType::None ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(fixDot, color, LV_PART_MAIN);
        lv_obj_set_style_text_color(fixLabel, color, LV_PART_MAIN);
    }
    setLabelText(fixLabel, fixText(s.fix));

    char text[96];
    if (std::isnan(s.hdop)) {
        setLabelText(accuracyLabel, "--");
    } else {
        char accuracy[24];
        formatAccuracy(estimatedAccuracyMeters(s.hdop), view.settings.units, accuracy, sizeof(accuracy));
        snprintf(text, sizeof(text), "\xC2\xB1%s", accuracy);
        setLabelText(accuracyLabel, text);
    }

    const float dops[3] = {s.hdop, s.vdop, s.pdop};
    for (int i = 0; i < 3; i++) {
        if (std::isnan(dops[i])) {
            setLabelText(dopValue[i], "--");
        } else {
            snprintf(text, sizeof(text), "%.1f", static_cast<double>(dops[i]));
            setLabelText(dopValue[i], text);
        }
        setLabelText(dopQuality[i], dopQualityWord(dops[i]));
    }

    snprintf(text, sizeof(text), "%d / %d", s.satellitesUsed, s.satellitesInView);
    setLabelText(satellitesLabel, text);

    char duration[24];
    if (s.hasTtff) {
        formatDuration(s.ttffMs / 1000, duration, sizeof(duration));
        setLabelText(ttffLabel, duration);
        setLabelText(ttffNote, "since app start");
    } else {
        formatDuration(s.listeningMs / 1000, duration, sizeof(duration));
        snprintf(text, sizeof(text), "searching %s", duration);
        setLabelText(ttffLabel, text);
        setLabelText(ttffNote, "since app start");
    }

    if (view.receiverName[0] == '\0') {
        snprintf(text, sizeof(text), "Receiver: unknown");
    } else {
        snprintf(text, sizeof(text), "Receiver: %s", view.receiverName);
    }
    setLabelText(receiverLabel, text);
}

void StatusTab::clear() {
    fixDot = nullptr;
    fixLabel = nullptr;
    accuracyLabel = nullptr;
    for (int i = 0; i < 3; i++) {
        dopValue[i] = nullptr;
        dopQuality[i] = nullptr;
    }
    satellitesLabel = nullptr;
    ttffLabel = nullptr;
    ttffNote = nullptr;
    receiverLabel = nullptr;
    lastFix = -1;
}

} // namespace gpsviewer
