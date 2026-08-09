/**
 * @file MenuView.cpp
 * @brief Menu view implementation
 */

#include "MenuView.h"
#include "TamaTac.h"

namespace {

void onStatsClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx) showStatsView(ctx);
}

void onSettingsClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx) showSettingsView(ctx);
}

void onCemeteryClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx) showCemeteryView(ctx);
}

void onAchievementsClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx) showAchievementsView(ctx);
}

lv_obj_t* addStyledListBtn(Context* ctx, lv_obj_t* menuList, const char* icon, const char* text, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_list_add_btn(menuList, icon, text);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ctx);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a4e), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4a4a7e), LV_STATE_PRESSED);
    lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
    return btn;
}

} // namespace

void menuViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx) {
    MenuViewState* state = &ctx->menuView;

    // Detect screen size for responsive layout
    // Use display resolution for reliable sizing (parent may not be laid out yet on first load)
    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    // Main wrapper
    state->mainWrapper = lv_obj_create(parentWidget);
    lv_obj_set_size(state->mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(state->mainWrapper, isSmall ? 4 : (isXLarge ? 16 : 8), 0);
    lv_obj_set_style_bg_opa(state->mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->mainWrapper, 0, 0);
    lv_obj_set_flex_flow(state->mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(state->mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(state->mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Menu list
    state->menuList = lv_list_create(state->mainWrapper);
    lv_obj_set_size(state->menuList, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(state->menuList, isSmall ? 2 : (isXLarge ? 8 : 4), 0);
    lv_obj_set_style_bg_color(state->menuList, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(state->menuList, isXLarge ? 2 : 1, 0);
    lv_obj_set_style_border_color(state->menuList, lv_color_hex(0x3a3a5e), 0);
    lv_obj_set_style_radius(state->menuList, isSmall ? 4 : (isXLarge ? 12 : 8), 0);

    addStyledListBtn(ctx, state->menuList, LV_SYMBOL_LIST, "Stats", onStatsClicked);
    addStyledListBtn(ctx, state->menuList, LV_SYMBOL_SETTINGS, "Settings", onSettingsClicked);
    addStyledListBtn(ctx, state->menuList, LV_SYMBOL_EYE_OPEN, "Cemetery", onCemeteryClicked);
    addStyledListBtn(ctx, state->menuList, LV_SYMBOL_OK, "Achievements", onAchievementsClicked);
}

void menuViewStop(Context* ctx) {
    ctx->menuView.mainWrapper = nullptr;
    ctx->menuView.menuList = nullptr;
}
