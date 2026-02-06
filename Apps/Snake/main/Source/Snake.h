/**
 * @file Snake.h
 * @brief Snake game app class for Tactility
 */
#pragma once

#include <tt_app.h>
#include <lvgl.h>
#include <TactilityCpp/App.h>

#include "SnakeUi.h"
#include "SnakeLogic.h"
#include "SnakeHelpers.h"

class Snake final : public App {

private:
    // UI element pointers (invalidated on hide, recreated on show)
    lv_obj_t* scoreLabel = nullptr;
    lv_obj_t* scoreWrapper = nullptr;
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* newGameWrapper = nullptr;
    lv_obj_t* gameObject = nullptr;

    // State tracking (persists across hide/show cycles)
    int32_t pendingSelection = -1;  // -1 = show selection, 1-3 = start game with difficulty
    bool shouldExit = false;
    bool showHelpOnShow = false;    // Show help dialog when onShow is called
    bool highScoresLoaded = false;
    int32_t currentDifficulty = -1;  // Track which difficulty is being played

    // Dialog launch IDs for tracking which dialog returned (only accessible to member functions)
    AppLaunchId selectionDialogId = 0;
    AppLaunchId gameOverDialogId = 0;
    AppLaunchId helpDialogId = 0;

    static void snakeEventCb(lv_event_t* e);
    static void newGameBtnEvent(lv_event_t* e);
    void createGame(lv_obj_t* parent, uint16_t cell_size, bool wallCollision, lv_obj_t* tb);
    void showSelectionDialog();
    void showHelpDialog();

public:

    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
    void onResult(AppHandle appHandle, void* _Nullable data, AppLaunchId launchId, AppResult result, BundleHandle resultData) override;
};
