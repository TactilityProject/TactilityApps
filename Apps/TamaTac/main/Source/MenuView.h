/**
 * @file MenuView.h
 * @brief Menu view for TamaTac navigation
 */
#pragma once

#include <lvgl.h>

struct Context;

struct MenuViewState {
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* menuList = nullptr;
};

void menuViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx);
void menuViewStop(Context* ctx);
