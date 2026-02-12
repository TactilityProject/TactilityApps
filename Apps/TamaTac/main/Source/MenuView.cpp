/**
 * @file MenuView.cpp
 * @brief Menu view implementation
 */

#include "MenuView.h"
#include "TamaTac.h"

void MenuView::onStart(lv_obj_t* parentWidget, TamaTac* appInstance) {
    parent = parentWidget;
    app = appInstance;

    // Detect screen size for responsive layout
    // Use display resolution for reliable sizing (parent may not be laid out yet on first load)
    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    // Main wrapper
    mainWrapper = lv_obj_create(parent);
    lv_obj_set_size(mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(mainWrapper, isSmall ? 4 : (isXLarge ? 16 : 8), 0);
    lv_obj_set_style_bg_opa(mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mainWrapper, 0, 0);
    lv_obj_set_flex_flow(mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Menu list
    menuList = lv_list_create(mainWrapper);
    lv_obj_set_size(menuList, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(menuList, isSmall ? 2 : (isXLarge ? 8 : 4), 0);
    lv_obj_set_style_bg_color(menuList, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(menuList, isXLarge ? 2 : 1, 0);
    lv_obj_set_style_border_color(menuList, lv_color_hex(0x3a3a5e), 0);
    lv_obj_set_style_radius(menuList, isSmall ? 4 : (isXLarge ? 12 : 8), 0);

    // Stats item
    lv_obj_t* statsBtn = lv_list_add_btn(menuList, LV_SYMBOL_LIST, "Stats");
    lv_obj_add_event_cb(statsBtn, onStatsClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_style_bg_color(statsBtn, lv_color_hex(0x2a2a4e), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(statsBtn, lv_color_hex(0x4a4a7e), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(statsBtn, lv_color_hex(0xFFFFFF), 0);

    // Settings item
    lv_obj_t* settingsBtn = lv_list_add_btn(menuList, LV_SYMBOL_SETTINGS, "Settings");
    lv_obj_add_event_cb(settingsBtn, onSettingsClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_style_bg_color(settingsBtn, lv_color_hex(0x2a2a4e), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(settingsBtn, lv_color_hex(0x4a4a7e), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(settingsBtn, lv_color_hex(0xFFFFFF), 0);

    // Cemetery item
    lv_obj_t* cemeteryBtn = lv_list_add_btn(menuList, LV_SYMBOL_EYE_OPEN, "Cemetery");
    lv_obj_add_event_cb(cemeteryBtn, onCemeteryClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_style_bg_color(cemeteryBtn, lv_color_hex(0x2a2a4e), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(cemeteryBtn, lv_color_hex(0x4a4a7e), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(cemeteryBtn, lv_color_hex(0xFFFFFF), 0);

    // Achievements item
    lv_obj_t* achieveBtn = lv_list_add_btn(menuList, LV_SYMBOL_OK, "Achievements");
    lv_obj_add_event_cb(achieveBtn, onAchievementsClicked, LV_EVENT_CLICKED, this);
    lv_obj_set_style_bg_color(achieveBtn, lv_color_hex(0x2a2a4e), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(achieveBtn, lv_color_hex(0x4a4a7e), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(achieveBtn, lv_color_hex(0xFFFFFF), 0);
}

void MenuView::onStop() {
    mainWrapper = nullptr;
    menuList = nullptr;
    parent = nullptr;
    app = nullptr;
}

void MenuView::onStatsClicked(lv_event_t* e) {
    MenuView* view = static_cast<MenuView*>(lv_event_get_user_data(e));
    if (view && view->app) {
        view->app->showStatsView();
    }
}

void MenuView::onSettingsClicked(lv_event_t* e) {
    MenuView* view = static_cast<MenuView*>(lv_event_get_user_data(e));
    if (view && view->app) {
        view->app->showSettingsView();
    }
}

void MenuView::onCemeteryClicked(lv_event_t* e) {
    MenuView* view = static_cast<MenuView*>(lv_event_get_user_data(e));
    if (view && view->app) {
        view->app->showCemeteryView();
    }
}

void MenuView::onAchievementsClicked(lv_event_t* e) {
    MenuView* view = static_cast<MenuView*>(lv_event_get_user_data(e));
    if (view && view->app) {
        view->app->showAchievementsView();
    }
}
