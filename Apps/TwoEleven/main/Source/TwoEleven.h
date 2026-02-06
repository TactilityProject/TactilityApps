#pragma once

#include <tt_app.h>

#include <lvgl.h>
#include <TactilityCpp/App.h>

#include "TwoElevenUi.h"
#include "TwoElevenLogic.h"
#include "TwoElevenHelpers.h"

class TwoEleven final : public App {

private:
    // UI element pointers (invalidated on hide, recreated on show)
    lv_obj_t* scoreLabel = nullptr;
    lv_obj_t* scoreWrapper = nullptr;
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* newGameWrapper = nullptr;
    lv_obj_t* gameObject = nullptr;

    // State tracking (persists across hide/show cycles)
    int32_t pendingSelection = -1;
    bool shouldExit = false;
    bool showHelpOnShow = false;
    int32_t currentGridSize = -1;
    bool highScoresLoaded = false;

    // Dialog launch IDs
    AppLaunchId selectionDialogId = 0;
    AppLaunchId gameOverDialogId = 0;
    AppLaunchId winDialogId = 0;
    AppLaunchId helpDialogId = 0;

    static void twoElevenEventCb(lv_event_t* e);
    static void newGameBtnEvent(lv_event_t* e);
    void createGame(lv_obj_t* parent, uint16_t size, lv_obj_t* toolbar);
    void showSelectionDialog();
    void showHelpDialog();

public:

    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
    void onResult(AppHandle appHandle, void* _Nullable data, AppLaunchId launchId, AppResult result, BundleHandle resultData) override;
};