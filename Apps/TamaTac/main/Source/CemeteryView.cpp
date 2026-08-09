/**
 * @file CemeteryView.cpp
 * @brief Pet Cemetery / Hall of Fame view implementation
 */

#include "CemeteryView.h"
#include "TamaTac.h"
#include <tactility/paths.h>
#include <tactility/preferences.h>
#include <cstdio>
#include <string>

namespace {

bool getPreferencesPath(std::string& outPath) {
    char root[128];
    if (paths_get_user_data_path(root, sizeof(root)) != ERROR_NONE) {
        return false;
    }
    outPath = std::string(root) + "/tamatac_cemetery.properties";
    return true;
}

} // namespace

void cemeteryViewLoadRecords(PetRecord records[CEMETERY_MAX_RECORDS]) {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;

    for (int i = 0; i < CEMETERY_MAX_RECORDS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "valid%d", i);
        bool valid = false;
        preferences_opt_bool(prefs, key, &valid);
        records[i].valid = valid;

        if (records[i].valid) {
            int32_t value;

            snprintf(key, sizeof(key), "pers%d", i);
            value = 0;
            preferences_opt_int32(prefs, key, &value);
            records[i].personality = static_cast<Personality>(value);

            snprintf(key, sizeof(key), "stage%d", i);
            value = 0;
            preferences_opt_int32(prefs, key, &value);
            records[i].stageReached = static_cast<LifeStage>(value);

            snprintf(key, sizeof(key), "age%d", i);
            value = 0;
            preferences_opt_int32(prefs, key, &value);
            records[i].ageHours = static_cast<uint16_t>(value);
        }
    }

    preferences_close(prefs);
}

void cemeteryViewRecordDeath(Personality personality, LifeStage stage, uint16_t ageHours) {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;

    auto getBool = [&](const char* key, bool defaultValue) {
        bool value = defaultValue;
        preferences_opt_bool(prefs, key, &value);
        return value;
    };
    auto getInt32 = [&](const char* key, int32_t defaultValue) {
        int32_t value = defaultValue;
        preferences_opt_int32(prefs, key, &value);
        return value;
    };

    // Shift existing records down (newest at index 0)
    for (int i = CEMETERY_MAX_RECORDS - 1; i > 0; i--) {
        char srcKey[16], dstKey[16];

        snprintf(srcKey, sizeof(srcKey), "valid%d", i - 1);
        snprintf(dstKey, sizeof(dstKey), "valid%d", i);
        bool srcValid = getBool(srcKey, false);
        preferences_put_bool(prefs, dstKey, srcValid);

        if (srcValid) {
            snprintf(srcKey, sizeof(srcKey), "pers%d", i - 1);
            snprintf(dstKey, sizeof(dstKey), "pers%d", i);
            preferences_put_int32(prefs, dstKey, getInt32(srcKey, 0));

            snprintf(srcKey, sizeof(srcKey), "stage%d", i - 1);
            snprintf(dstKey, sizeof(dstKey), "stage%d", i);
            preferences_put_int32(prefs, dstKey, getInt32(srcKey, 0));

            snprintf(srcKey, sizeof(srcKey), "age%d", i - 1);
            snprintf(dstKey, sizeof(dstKey), "age%d", i);
            preferences_put_int32(prefs, dstKey, getInt32(srcKey, 0));
        }
    }

    // Write new record at index 0
    preferences_put_bool(prefs, "valid0", true);
    preferences_put_int32(prefs, "pers0", static_cast<int32_t>(personality));
    preferences_put_int32(prefs, "stage0", static_cast<int32_t>(stage));
    preferences_put_int32(prefs, "age0", static_cast<int32_t>(ageHours));

    preferences_close(prefs);
}

void cemeteryViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx) {
    CemeteryViewState* state = &ctx->cemeteryView;

    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    int padAll = isSmall ? 4 : (isXLarge ? 16 : 8);
    int padRow = isSmall ? 4 : (isXLarge ? 12 : 6);

    state->mainWrapper = lv_obj_create(parentWidget);
    lv_obj_set_size(state->mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(state->mainWrapper, padAll, 0);
    lv_obj_set_style_pad_row(state->mainWrapper, padRow, 0);
    lv_obj_set_style_bg_opa(state->mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->mainWrapper, 0, 0);
    lv_obj_set_flex_flow(state->mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state->mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Title
    lv_obj_t* title = lv_label_create(state->mainWrapper);
    lv_label_set_text(title, "Pet Cemetery");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);

    // Load records
    PetRecord records[CEMETERY_MAX_RECORDS];
    cemeteryViewLoadRecords(records);

    bool anyRecords = false;
    for (int i = 0; i < CEMETERY_MAX_RECORDS; i++) {
        if (!records[i].valid) continue;
        anyRecords = true;

        lv_obj_t* row = lv_obj_create(state->mainWrapper);
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
        lv_obj_t* emptyLabel = lv_label_create(state->mainWrapper);
        lv_label_set_text(emptyLabel, "No records yet.");
        lv_obj_set_style_text_color(emptyLabel, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);
    }
}

void cemeteryViewStop(Context* ctx) {
    ctx->cemeteryView.mainWrapper = nullptr;
}
