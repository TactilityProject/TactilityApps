#pragma once

#include <lvgl.h>
#include <stdint.h>

struct Context {
    uint32_t appInstanceId;

    lv_obj_t* displayLabel = nullptr;
    lv_obj_t* resultLabel = nullptr;
    char formulaBuffer[128] = {0}; // Stores the full input expression
    bool newInput = true;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void calculatorCreateWidgets(lv_obj_t* parent, void* userData);
