/**
 * @file Snake.h
 * @brief Snake game app for Tactility
 */
#pragma once

#include "SnakeUi.h"
#include "SnakeLogic.h"
#include "SnakeHelpers.h"

#include <app/instance.h>
#include <tactility/concurrent/task_event_group.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>

#include <atomic>

// Selection dialog indices (0 = How to Play, 1-4 = difficulties) - shared between Snake.cpp
// (which builds the dialog) and main.cpp (which interprets its result).
constexpr int32_t SNAKE_SELECTION_HOW_TO_PLAY = 0;
constexpr int32_t SNAKE_SELECTION_EASY = 1;
constexpr int32_t SNAKE_SELECTION_MEDIUM = 2;
constexpr int32_t SNAKE_SELECTION_HARD = 3;
constexpr int32_t SNAKE_SELECTION_HELL = 4;

/** The dialog that is shown, if any */
enum class GameDialog { None, Selection, Help, GameOver };

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    // UI element pointers (invalidated on rebuild, recreated in snakeCreateWidgets)
    lv_obj_t* scoreLabel = nullptr;
    lv_obj_t* scoreWrapper = nullptr;
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* gameObject = nullptr;

    // State tracking (persists across widget rebuilds)
    bool highScoresLoaded = false;
    int32_t currentDifficulty = -1; // Which difficulty is being played, -1 = none

    // High scores for each difficulty (loaded from preferences on first widget build)
    int32_t highScoreEasy = 0;
    int32_t highScoreMedium = 0;
    int32_t highScoreHard = 0;
    int32_t highScoreHell = 0;

    // The dialog's result is handled on the app task, which dialogBit wakes
    TaskEventGroup* eventGroup = nullptr;
    uint32_t dialogBit = 0;
    std::atomic<GameDialog> dialog { GameDialog::None };
    std::atomic<int32_t> dialogResult { 0 };
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void snakeCreateWidgets(lv_obj_t* parent, void* userData);

/** Nothing to release beyond widget-tracking state - call once, after the window has been torn
 *  down. */
void snakeTeardown(Context* ctx);

// The functions below are driven by main.cpp: once at startup, and again after each dialog result.

/** Opens the difficulty/help selection dialog. Doesn't touch any widgets - safe to call without
 *  the LVGL lock. */
void snakeShowSelectionDialog(Context* ctx);

/** Opens the "How to Play" alert dialog. Doesn't touch any widgets - safe to call without the
 *  LVGL lock. */
void snakeShowHelpDialog(Context* ctx);

/** Tears down any previous game and starts a fresh one at @a difficulty (one of
 *  SNAKE_SELECTION_EASY..SNAKE_SELECTION_HELL). Touches widgets - caller must hold the LVGL
 *  lock. */
void snakeStartGame(Context* ctx, int32_t difficulty);

/** Tears down the current game (if any), leaving mainWrapper empty and currentDifficulty back
 *  to -1. Touches widgets - caller must hold the LVGL lock. */
void snakeClearGame(Context* ctx);

/** Opens an alert dialog with an OK button, whose result arrives as @a kind. Doesn't touch any widgets. */
void snakeShowAlert(Context* ctx, GameDialog kind, const char* title, const char* message);
