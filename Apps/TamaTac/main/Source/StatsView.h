/**
 * @file StatsView.h
 * @brief Stats detail view for TamaTac
 */
#pragma once

#include <lvgl.h>
#include "PetLogic.h"

struct Context;

struct StatsViewState {
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* titleLabel = nullptr;
    lv_obj_t* statusLabel = nullptr;

    // Individual stat value labels
    lv_obj_t* hungerValue = nullptr;
    lv_obj_t* happyValue = nullptr;
    lv_obj_t* healthValue = nullptr;
    lv_obj_t* energyValue = nullptr;
    lv_obj_t* cleanValue = nullptr;
    lv_obj_t* personalityValue = nullptr;
};

void statsViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx);
void statsViewStop(Context* ctx);

void statsViewUpdateStats(Context* ctx);
