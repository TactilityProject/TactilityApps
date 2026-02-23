/**
 * @file Achievements.cpp
 * @brief Achievement system implementation
 */

#include "Achievements.h"
#include "TamaTac.h"
#include <TactilityCpp/Preferences.h>
#include <cstdio>

static constexpr const char* PREF_NS = "TamaTacAch";

static const AchievementInfo achievementInfos[] = {
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

const AchievementInfo& AchievementsView::getInfo(AchievementId id) {
    int idx = static_cast<int>(id);
    if (idx < 0 || idx >= static_cast<int>(AchievementId::COUNT)) {
        idx = 0;
    }
    return achievementInfos[idx];
}

uint16_t AchievementsView::loadAchievements() {
    Preferences prefs(PREF_NS);
    return static_cast<uint16_t>(prefs.getInt32("bits", 0));
}

void AchievementsView::saveAchievements(uint16_t bits) {
    Preferences prefs(PREF_NS);
    prefs.putInt32("bits", static_cast<int32_t>(bits));
}

bool AchievementsView::hasAchievement(uint16_t bits, AchievementId id) {
    return (bits >> static_cast<uint8_t>(id)) & 1;
}

void AchievementsView::unlock(AchievementId id) {
    if (static_cast<int>(id) >= static_cast<int>(AchievementId::COUNT)) {
        return;
    }
    uint16_t bits = loadAchievements();
    uint16_t mask = static_cast<uint16_t>(1 << static_cast<int>(id));
    if (!(bits & mask)) {
        bits |= mask;
        saveAchievements(bits);
    }
}

int AchievementsView::countUnlocked(uint16_t bits) {
    int count = 0;
    for (int i = 0; i < static_cast<int>(AchievementId::COUNT); i++) {
        if ((bits >> i) & 1) count++;
    }
    return count;
}

uint16_t AchievementsView::loadCleanCount() {
    Preferences prefs(PREF_NS);
    return static_cast<uint16_t>(prefs.getInt32("cleanCnt", 0));
}

void AchievementsView::incrementCleanCount() {
    Preferences prefs(PREF_NS);
    int32_t count = prefs.getInt32("cleanCnt", 0) + 1;
    prefs.putInt32("cleanCnt", count);
    if (count >= 10) {
        unlock(AchievementId::CleanFreak);
    }
}

void AchievementsView::onStart(lv_obj_t* parentWidget, TamaTac* appInstance) {
    parent = parentWidget;
    app = appInstance;

    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    int padAll = isSmall ? 4 : (isXLarge ? 16 : 8);
    int padRow = isSmall ? 2 : (isXLarge ? 8 : 4);

    mainWrapper = lv_obj_create(parent);
    lv_obj_set_size(mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(mainWrapper, padAll, 0);
    lv_obj_set_style_pad_row(mainWrapper, padRow, 0);
    lv_obj_set_style_bg_opa(mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mainWrapper, 0, 0);
    lv_obj_set_flex_flow(mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    uint16_t bits = loadAchievements();
    int unlocked = countUnlocked(bits);

    // Title with count
    lv_obj_t* title = lv_label_create(mainWrapper);
    char titleText[48];
    snprintf(titleText, sizeof(titleText), "Achievements %d/%d", unlocked, static_cast<int>(AchievementId::COUNT));
    lv_label_set_text(title, titleText);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);

    // Achievement list
    for (int i = 0; i < static_cast<int>(AchievementId::COUNT); i++) {
        AchievementId id = static_cast<AchievementId>(i);
        bool has = hasAchievement(bits, id);
        const AchievementInfo& info = getInfo(id);

        lv_obj_t* row = lv_obj_create(mainWrapper);
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

void AchievementsView::onStop() {
    mainWrapper = nullptr;
    parent = nullptr;
    app = nullptr;
}
