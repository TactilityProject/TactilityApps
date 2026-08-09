/**
 * @file TwoEleven.h
 * @brief 2048 game app for Tactility
 */
#pragma once

#include "TwoElevenUi.h"
#include "TwoElevenLogic.h"
#include "TwoElevenHelpers.h"

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>

// Selection dialog indices (0 = How to Play, 1-4 = grid sizes) - shared between TwoEleven.cpp
// (which builds the dialog) and main.cpp (which interprets its APP_EVENT_RESULT).
constexpr int32_t TWOELEVEN_SELECTION_HOW_TO_PLAY = 0;
constexpr int32_t TWOELEVEN_SELECTION_3X3 = 1;
constexpr int32_t TWOELEVEN_SELECTION_4X4 = 2;
constexpr int32_t TWOELEVEN_SELECTION_5X5 = 3;
constexpr int32_t TWOELEVEN_SELECTION_6X6 = 4;

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    // UI element pointers (invalidated on rebuild, recreated in twoElevenCreateWidgets)
    lv_obj_t* scoreLabel = nullptr;
    lv_obj_t* scoreWrapper = nullptr;
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* newGameWrapper = nullptr;
    lv_obj_t* gameObject = nullptr;

    // State tracking (persists across widget rebuilds)
    int32_t pendingSelection = -1;
    bool shouldExit = false;
    bool showHelpOnShow = false;    // Show help dialog next time widgets are (re)built
    int32_t currentGridSize = -1;   // Which grid size is being played, -1 = none
    bool highScoresLoaded = false;

    // High scores for each grid size (loaded from preferences on first widget build)
    int32_t highScore3x3 = 0;
    int32_t highScore4x4 = 0;
    int32_t highScore5x5 = 0;
    int32_t highScore6x6 = 0;

    // Dialog launch ids for tracking which dialog returned
    uint32_t selectionDialogId = 0;
    uint32_t gameOverDialogId = 0;
    uint32_t helpDialogId = 0;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void twoElevenCreateWidgets(lv_obj_t* parent, void* userData);

/** Nothing to release beyond widget-tracking state - call once, after the window has been torn
 *  down. */
void twoElevenTeardown(Context* ctx);
