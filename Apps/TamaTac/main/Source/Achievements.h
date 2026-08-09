/**
 * @file Achievements.h
 * @brief Achievement system for TamaTac
 */
#pragma once

#include <lvgl.h>
#include <cstdint>

struct Context;

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

struct AchievementsViewState {
    lv_obj_t* mainWrapper = nullptr;
};

void achievementsViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx);
void achievementsViewStop(Context* ctx);

// Bitfield operations
uint16_t achievementsLoad();
void achievementsSave(uint16_t bits);
bool achievementsHas(uint16_t bits, AchievementId id);
void achievementsUnlock(AchievementId id);
int achievementsCountUnlocked(uint16_t bits);
uint16_t achievementsLoadCleanCount();
void achievementsIncrementCleanCount();

const AchievementInfo& achievementsGetInfo(AchievementId id);
