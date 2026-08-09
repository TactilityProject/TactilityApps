/**
 * @file PatternGame.h
 * @brief Simon Says mini-game for TamaTac Play action
 */
#pragma once

#include <lvgl.h>
#include <cstdint>

struct Context;

struct PatternGameState {
    // UI elements
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* buttons[4] = {};
    lv_timer_t* sequenceTimer = nullptr;
    lv_timer_t* delayTimer = nullptr;

    // Game state
    uint8_t pattern[8] = {};
    int patternLength = 3;
    int showIndex = 0;
    bool showPhase = false;
    int inputIndex = 0;
    int round = 0;
    bool acceptingInput = false;

    // Delay action tracking
    enum class DelayAction { StartSequence, NextRound, EndGame };
    DelayAction pendingAction = DelayAction::StartSequence;
    bool gameWon = false;
};

constexpr int PATTERN_GAME_MAX_ROUNDS = 3;

void patternGameCreateWidgets(lv_obj_t* parent, Context* ctx);
void patternGameStop(Context* ctx);
