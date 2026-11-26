/**
 * @file TwoEleven.h
 * @brief 2048 game app for Tactility
 */
#pragma once

#include "TwoElevenUi.h"
#include "TwoElevenLogic.h"
#include "TwoElevenHelpers.h"

#include <app/instance.h>
#include <tactility/concurrent/task_event_group.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>

#include <atomic>

// Selection dialog indices (0 = How to Play, 1-4 = grid sizes) - shared between TwoEleven.cpp
// (which builds the dialog) and main.cpp (which interprets its result).
constexpr int32_t TWOELEVEN_SELECTION_HOW_TO_PLAY = 0;
constexpr int32_t TWOELEVEN_SELECTION_3X3 = 1;
constexpr int32_t TWOELEVEN_SELECTION_4X4 = 2;
constexpr int32_t TWOELEVEN_SELECTION_5X5 = 3;
constexpr int32_t TWOELEVEN_SELECTION_6X6 = 4;

/** The dialog that is shown, if any */
enum class GameDialog { None, Selection, Help, GameOver };

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    // UI element pointers (invalidated on rebuild, recreated in twoElevenCreateWidgets)
    lv_obj_t* scoreLabel = nullptr;
    lv_obj_t* scoreWrapper = nullptr;
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* gameObject = nullptr;

    // State tracking (persists across widget rebuilds)
    int32_t currentGridSize = -1;   // Which grid size is being played, -1 = none
    bool highScoresLoaded = false;

    // High scores for each grid size (loaded from preferences on first widget build)
    int32_t highScore3x3 = 0;
    int32_t highScore4x4 = 0;
    int32_t highScore5x5 = 0;
    int32_t highScore6x6 = 0;

    // The dialog's result is handled on the app task, which dialogBit wakes
    TaskEventGroup* eventGroup = nullptr;
    uint32_t dialogBit = 0;
    std::atomic<GameDialog> dialog { GameDialog::None };
    std::atomic<int32_t> dialogResult { 0 };
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void twoElevenCreateWidgets(lv_obj_t* parent, void* userData);

/** Nothing to release beyond widget-tracking state - call once, after the window has been torn
 *  down. */
void twoElevenTeardown(Context* ctx);

// The functions below are driven by main.cpp: once at startup, and again after each dialog result.

/** Opens the grid-size/help selection dialog. Doesn't touch any widgets - safe to call without
 *  the LVGL lock. */
void twoElevenShowSelectionDialog(Context* ctx);

/** Opens the "How to Play" alert dialog. Doesn't touch any widgets - safe to call without the
 *  LVGL lock. */
void twoElevenShowHelpDialog(Context* ctx);

/** Tears down any previous game and starts a fresh one at @a gridSize (one of
 *  TWOELEVEN_SELECTION_3X3..TWOELEVEN_SELECTION_6X6). Touches widgets - caller must hold the
 *  LVGL lock. */
void twoElevenStartGame(Context* ctx, int32_t gridSize);

/** Tears down the current game (if any), leaving mainWrapper empty and currentGridSize back to
 *  -1. Touches widgets - caller must hold the LVGL lock. */
void twoElevenClearGame(Context* ctx);

/** Opens an alert dialog with an OK button, whose result arrives as @a kind. Doesn't touch any widgets. */
void twoElevenShowAlert(Context* ctx, GameDialog kind, const char* title, const char* message);
