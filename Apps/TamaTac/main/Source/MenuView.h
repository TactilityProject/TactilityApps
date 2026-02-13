/**
 * @file MenuView.h
 * @brief Menu view for TamaTac navigation
 */
#pragma once

#include <lvgl.h>

class TamaTac;

class MenuView {
private:
    TamaTac* app = nullptr;
    lv_obj_t* parent = nullptr;

    // UI elements
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* menuList = nullptr;

public:
    MenuView() = default;
    ~MenuView() = default;

    MenuView(const MenuView&) = delete;
    MenuView& operator=(const MenuView&) = delete;

    void onStart(lv_obj_t* parentWidget, TamaTac* appInstance);
    void onStop();

    lv_obj_t* addStyledListBtn(const char* icon, const char* text, lv_event_cb_t cb);

private:
    // Static event handlers
    static void onStatsClicked(lv_event_t* e);
    static void onSettingsClicked(lv_event_t* e);
    static void onCemeteryClicked(lv_event_t* e);
    static void onAchievementsClicked(lv_event_t* e);
};
