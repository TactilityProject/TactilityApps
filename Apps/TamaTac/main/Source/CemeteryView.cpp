/**
 * @file CemeteryView.cpp
 * @brief Pet Cemetery / Hall of Fame view implementation
 */

#include "CemeteryView.h"
#include "TamaTac.h"
#include <TactilityCpp/Preferences.h>
#include <cstdio>

static constexpr const char* PREF_NS = "TamaTacCem";

void CemeteryView::loadRecords(PetRecord records[MAX_RECORDS]) {
    Preferences prefs(PREF_NS);

    for (int i = 0; i < MAX_RECORDS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "valid%d", i);
        records[i].valid = prefs.getBool(key, false);

        if (records[i].valid) {
            snprintf(key, sizeof(key), "pers%d", i);
            records[i].personality = static_cast<Personality>(prefs.getInt32(key, 0));

            snprintf(key, sizeof(key), "stage%d", i);
            records[i].stageReached = static_cast<LifeStage>(prefs.getInt32(key, 0));

            snprintf(key, sizeof(key), "age%d", i);
            records[i].ageHours = static_cast<uint16_t>(prefs.getInt32(key, 0));
        }
    }
}

void CemeteryView::recordDeath(Personality personality, LifeStage stage, uint16_t ageHours) {
    Preferences prefs(PREF_NS);

    // Shift existing records down (newest at index 0)
    for (int i = MAX_RECORDS - 1; i > 0; i--) {
        char srcKey[16], dstKey[16];

        snprintf(srcKey, sizeof(srcKey), "valid%d", i - 1);
        snprintf(dstKey, sizeof(dstKey), "valid%d", i);
        bool srcValid = prefs.getBool(srcKey, false);
        prefs.putBool(dstKey, srcValid);

        if (srcValid) {
            snprintf(srcKey, sizeof(srcKey), "pers%d", i - 1);
            snprintf(dstKey, sizeof(dstKey), "pers%d", i);
            prefs.putInt32(dstKey, prefs.getInt32(srcKey, 0));

            snprintf(srcKey, sizeof(srcKey), "stage%d", i - 1);
            snprintf(dstKey, sizeof(dstKey), "stage%d", i);
            prefs.putInt32(dstKey, prefs.getInt32(srcKey, 0));

            snprintf(srcKey, sizeof(srcKey), "age%d", i - 1);
            snprintf(dstKey, sizeof(dstKey), "age%d", i);
            prefs.putInt32(dstKey, prefs.getInt32(srcKey, 0));
        }
    }

    // Write new record at index 0
    prefs.putBool("valid0", true);
    prefs.putInt32("pers0", static_cast<int32_t>(personality));
    prefs.putInt32("stage0", static_cast<int32_t>(stage));
    prefs.putInt32("age0", static_cast<int32_t>(ageHours));
}

void CemeteryView::onStart(lv_obj_t* parentWidget, TamaTac* appInstance) {
    parent = parentWidget;
    app = appInstance;

    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    int padAll = isSmall ? 4 : (isXLarge ? 16 : 8);
    int padRow = isSmall ? 4 : (isXLarge ? 12 : 6);

    mainWrapper = lv_obj_create(parent);
    lv_obj_set_size(mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(mainWrapper, padAll, 0);
    lv_obj_set_style_pad_row(mainWrapper, padRow, 0);
    lv_obj_set_style_bg_opa(mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mainWrapper, 0, 0);
    lv_obj_set_flex_flow(mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Title
    lv_obj_t* title = lv_label_create(mainWrapper);
    lv_label_set_text(title, "Pet Cemetery");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);

    // Load records
    PetRecord records[MAX_RECORDS];
    loadRecords(records);

    bool anyRecords = false;
    for (int i = 0; i < MAX_RECORDS; i++) {
        if (!records[i].valid) continue;
        anyRecords = true;

        lv_obj_t* row = lv_obj_create(mainWrapper);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, isSmall ? 4 : (isXLarge ? 12 : 6), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a4e), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, isSmall ? 4 : (isXLarge ? 10 : 6), 0);

        char text[80];
        int days = records[i].ageHours / 24;
        int hours = records[i].ageHours % 24;
        snprintf(text, sizeof(text), "#%d  %s  %s  %dd %dh",
                 i + 1,
                 personalityToString(records[i].personality),
                 lifeStageToString(records[i].stageReached),
                 days, hours);

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, lv_color_hex(0xCCCCCC), 0);
    }

    if (!anyRecords) {
        lv_obj_t* emptyLabel = lv_label_create(mainWrapper);
        lv_label_set_text(emptyLabel, "No records yet.");
        lv_obj_set_style_text_color(emptyLabel, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);
    }
}

void CemeteryView::onStop() {
    mainWrapper = nullptr;
    parent = nullptr;
    app = nullptr;
}
