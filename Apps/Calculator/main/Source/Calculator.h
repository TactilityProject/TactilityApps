#pragma once

#include <lvgl.h>
#include <stdint.h>
#include <string>

struct Context {
    uint32_t appInstanceId;

    lv_obj_t* displayLabel = nullptr;
    lv_obj_t* historyLabel = nullptr;
    lv_obj_t* resultLabel = nullptr;
    char formulaBuffer[128] = {0}; // Stores the full input expression
    bool newInput = true;
    bool showingResult = false; // the input is the last result, untouched
    char lastResult[32] = {0}; // Last computed result, empty if none available
    bool hasLastResult = false;
    // The last calculation and its result, as shown above the input
    std::string history;
    std::string historyResult;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void calculatorCreateWidgets(lv_obj_t* parent, void* userData);
