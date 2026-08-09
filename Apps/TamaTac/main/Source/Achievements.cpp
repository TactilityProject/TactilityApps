/**
 * @file Achievements.cpp
 * @brief Achievement system implementation
 */

#include "Achievements.h"
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
    outPath = std::string(root) + "/tamatac_achievements.properties";
    return true;
}

const AchievementInfo achievementInfos[] = {
    {"First Feed",   "Feed your pet"},
    {"First Play",   "Play a mini-game"},
    {"First Cure",   "Cure sickness"},
    {"Baby Steps",   "Evolve to Baby"},
    {"Growing Up",   "Evolve to Teen"},
    {"All Grown Up", "Evolve to Adult"},
    {"Wise Elder",   "Evolve to Elder"},
    {"Perfect Pet",  "All stats >= 90"},
    {"Survivor",     "Pet lives 24h"},
    {"Pro Gamer",    "Perfect mini-game"},
    {"Clean Freak",  "Clean 10 times"},
    {"Night Owl",    "Play at night"},
};

} // namespace

const AchievementInfo& achievementsGetInfo(AchievementId id) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= static_cast<int>(AchievementId::COUNT)) {
        idx = 0;
    }
    return achievementInfos[idx];
}

uint16_t achievementsLoad() {
    std::string path;
    if (!getPreferencesPath(path)) return 0;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return 0;
    int32_t bits = 0;
    preferences_opt_int32(prefs, "bits", &bits);
    preferences_close(prefs);
    return static_cast<uint16_t>(bits);
}

void achievementsSave(uint16_t bits) {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;
    preferences_put_int32(prefs, "bits", static_cast<int32_t>(bits));
    preferences_close(prefs);
}

bool achievementsHas(uint16_t bits, AchievementId id) {
    return (bits >> static_cast<uint8_t>(id)) & 1;
}

void achievementsUnlock(AchievementId id) {
    if (static_cast<int>(id) >= static_cast<int>(AchievementId::COUNT)) {
        return;
    }
    uint16_t bits = achievementsLoad();
    uint16_t mask = static_cast<uint16_t>(1 << static_cast<int>(id));
    if (!(bits & mask)) {
        bits |= mask;
        achievementsSave(bits);
    }
}

int achievementsCountUnlocked(uint16_t bits) {
    int count = 0;
    for (int i = 0; i < static_cast<int>(AchievementId::COUNT); i++) {
        if ((bits >> i) & 1) count++;
    }
    return count;
}

uint16_t achievementsLoadCleanCount() {
    std::string path;
    if (!getPreferencesPath(path)) return 0;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return 0;
    int32_t count = 0;
    preferences_opt_int32(prefs, "cleanCnt", &count);
    preferences_close(prefs);
    return static_cast<uint16_t>(count);
}

void achievementsIncrementCleanCount() {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;
    int32_t count = 0;
    preferences_opt_int32(prefs, "cleanCnt", &count);
    count += 1;
    preferences_put_int32(prefs, "cleanCnt", count);
    preferences_close(prefs);

    if (count >= 10) {
        achievementsUnlock(AchievementId::CleanFreak);
    }
}

void achievementsViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx) {
    AchievementsViewState* state = &ctx->achievementsView;

    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    int padAll = isSmall ? 4 : (isXLarge ? 16 : 8);
    int padRow = isSmall ? 2 : (isXLarge ? 8 : 4);

    state->mainWrapper = lv_obj_create(parentWidget);
    lv_obj_set_size(state->mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(state->mainWrapper, padAll, 0);
    lv_obj_set_style_pad_row(state->mainWrapper, padRow, 0);
    lv_obj_set_style_bg_opa(state->mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->mainWrapper, 0, 0);
    lv_obj_set_flex_flow(state->mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state->mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    uint16_t bits = achievementsLoad();
    int unlocked = achievementsCountUnlocked(bits);

    // Title with count
    lv_obj_t* title = lv_label_create(state->mainWrapper);
    char titleText[48];
    snprintf(titleText, sizeof(titleText), "Achievements %d/%d", unlocked, static_cast<int>(AchievementId::COUNT));
    lv_label_set_text(title, titleText);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);

    // Achievement list
    for (int i = 0; i < static_cast<int>(AchievementId::COUNT); i++) {
        AchievementId id = static_cast<AchievementId>(i);
        bool has = achievementsHas(bits, id);
        const AchievementInfo& info = achievementsGetInfo(id);

        lv_obj_t* row = lv_obj_create(state->mainWrapper);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(row, isSmall ? 2 : (isXLarge ? 8 : 4), 0);
        lv_obj_set_style_bg_color(row, has ? lv_color_hex(0x2a4a2e) : lv_color_hex(0x2a2a4e), 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, isSmall ? 2 : (isXLarge ? 8 : 4), 0);

        char text[80];
        snprintf(text, sizeof(text), "%s %s - %s",
                 has ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE,
                 info.name, info.description);

        lv_obj_t* label = lv_label_create(row);
        lv_label_set_text(label, text);
        lv_obj_set_style_text_color(label, has ? lv_color_hex(0x88FF88) : lv_color_hex(0x888888), 0);
    }
}

void achievementsViewStop(Context* ctx) {
    ctx->achievementsView.mainWrapper = nullptr;
}
