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

    static void snakeEventCb(lv_event_t* e);
    static void newGameBtnEvent(lv_event_t* e);
    static void createGame(lv_obj_t* parent, uint16_t cell_size, bool wallCollision, lv_obj_t* toolbar);

public:

    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
    void onDestroy(AppHandle context) override;
    void onResult(AppHandle appHandle, void* _Nullable data, AppLaunchId launchId, AppResult result, BundleHandle resultData) override;
};
