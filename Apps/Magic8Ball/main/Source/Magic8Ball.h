#pragma once

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    // UI pointers (nulled in teardown)
    lv_obj_t* answerLabel = nullptr;
    lv_obj_t* hintLabel = nullptr;
    lv_obj_t* ballObj = nullptr;

    // State
    int lastIdx = -1;
    bool seeded = false;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void magic8BallCreateWidgets(lv_obj_t* parent, void* userData);

/** Releases widget-tracking state. Call once, after the window has been torn down. */
void magic8BallTeardown(Context* ctx);
