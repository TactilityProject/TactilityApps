/**
 * @file Snake.h
 * @brief Snake game app for Tactility
 */
#pragma once

#include "SnakeUi.h"
#include "SnakeLogic.h"
#include "SnakeHelpers.h"

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>

// Selection dialog indices (0 = How to Play, 1-4 = difficulties) - shared between Snake.cpp
// (which builds the dialog) and main.cpp (which interprets its APP_EVENT_RESULT).
constexpr int32_t SNAKE_SELECTION_HOW_TO_PLAY = 0;
constexpr int32_t SNAKE_SELECTION_EASY = 1;
constexpr int32_t SNAKE_SELECTION_MEDIUM = 2;
constexpr int32_t SNAKE_SELECTION_HARD = 3;
constexpr int32_t SNAKE_SELECTION_HELL = 4;

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    // UI element pointers (invalidated on rebuild, recreated in snakeCreateWidgets)
    lv_obj_t* scoreLabel = nullptr;
    lv_obj_t* scoreWrapper = nullptr;
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* newGameWrapper = nullptr;
    lv_obj_t* gameObject = nullptr;

    // State tracking (persists across widget rebuilds)
    int32_t pendingSelection = -1;  // -1 = show selection, 1-4 = start game with difficulty
    bool shouldExit = false;
    bool showHelpOnShow = false;    // Show help dialog next time widgets are (re)built
    bool highScoresLoaded = false;
    int32_t currentDifficulty = -1; // Which difficulty is being played, -1 = none

    // High scores for each difficulty (loaded from preferences on first widget build)
    int32_t highScoreEasy = 0;
    int32_t highScoreMedium = 0;
    int32_t highScoreHard = 0;
    int32_t highScoreHell = 0;

    // Dialog launch IDs for tracking which dialog returned
    uint32_t selectionDialogId = 0;
    uint32_t gameOverDialogId = 0;
    uint32_t helpDialogId = 0;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void snakeCreateWidgets(lv_obj_t* parent, void* userData);

/** Nothing to release beyond widget-tracking state - call once, after the window has been torn
 *  down. */
void snakeTeardown(Context* ctx);
