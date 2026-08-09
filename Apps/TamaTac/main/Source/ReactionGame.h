/**
 * @file ReactionGame.h
 * @brief Reaction time mini-game for TamaTac Play action
 */
#pragma once

#include <lvgl.h>
#include <cstdint>

struct Context;

struct ReactionGameState {
    // UI elements
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* targetBtn = nullptr;
    lv_timer_t* delayTimer = nullptr;

    // State machine
    enum class Phase { WaitForTarget, TargetShown, RoundResult, Done };
    Phase phase = Phase::WaitForTarget;

    // Game state
    int round = 0;
    int score = 0;               // 0-3 based on reaction quality
    uint32_t targetShowTime = 0; // When target appeared (millis)
};

constexpr int REACTION_GAME_MAX_ROUNDS = 3;

void reactionGameCreateWidgets(lv_obj_t* parent, Context* ctx);
void reactionGameStop(Context* ctx);
