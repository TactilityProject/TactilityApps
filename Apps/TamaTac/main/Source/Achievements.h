/**
 * @file Achievements.h
 * @brief Achievement system for TamaTac
 */
#pragma once

#include <lvgl.h>
#include <cstdint>

class TamaTac;

// Achievement IDs (bit positions in uint16_t bitfield)
enum class AchievementId : uint8_t {
    FirstFeed = 0,      // Feed pet for the first time
    FirstPlay = 1,      // Play a mini-game
    FirstCure = 2,      // Cure sickness
    ReachBaby = 3,      // Evolve to Baby
    ReachTeen = 4,      // Evolve to Teen
    ReachAdult = 5,     // Evolve to Adult
    ReachElder = 6,     // Evolve to Elder
    FullStats = 7,      // All primary stats >= 90
    Survivor24h = 8,    // Pet survives 24 hours
    PerfectGame = 9,    // Win a mini-game with perfect score
    CleanFreak = 10,    // Clean 10 times total
    NightOwl = 11,      // Play during night phase
    COUNT = 12
};

struct AchievementInfo {
    const char* name;
    const char* description;
};

class AchievementsView {
private:
    TamaTac* app = nullptr;
    lv_obj_t* parent = nullptr;
    lv_obj_t* mainWrapper = nullptr;

public:
    AchievementsView() = default;
    ~AchievementsView() = default;
    AchievementsView(const AchievementsView&) = delete;
    AchievementsView& operator=(const AchievementsView&) = delete;

    void onStart(lv_obj_t* parentWidget, TamaTac* appInstance);
    void onStop();

    // Bitfield operations
    static uint16_t loadAchievements();
    static void saveAchievements(uint16_t bits);
    static bool hasAchievement(uint16_t bits, AchievementId id);
    static void unlock(AchievementId id);
    static int countUnlocked(uint16_t bits);
    static uint16_t loadCleanCount();
    static void incrementCleanCount();

    static const AchievementInfo& getInfo(AchievementId id);
};
