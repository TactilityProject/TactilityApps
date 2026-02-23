/**
 * @file StatsView.h
 * @brief Stats detail view for TamaTac
 */
#pragma once

#include <lvgl.h>
#include "PetLogic.h"

class TamaTac;

class StatsView {
private:
    TamaTac* app = nullptr;
    lv_obj_t* parent = nullptr;

    // UI elements
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

    // Helper to create stat row
    lv_obj_t* createStatRow(lv_obj_t* parentContainer, const char* labelText, lv_color_t color, bool isXLarge = false);

public:
    void onStart(lv_obj_t* parentWidget, TamaTac* appInstance);
    void onStop();

    void updateStats(PetLogic* petLogic);
};
