#pragma once

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    static constexpr int MAX_TODOS = 50;
    static constexpr int MAX_TEXT_LEN = 128;

    struct TodoItem {
        char text[MAX_TEXT_LEN];
        bool done;
    };

    // UI pointers (nulled in teardown)
    lv_obj_t* list = nullptr;
    lv_obj_t* inputRow = nullptr;
    lv_obj_t* inputTa = nullptr;
    lv_obj_t* countLabel = nullptr;

    // Data
    TodoItem items[MAX_TODOS] = {};
    int count = 0;
    bool rebuildPending = false;
    lv_timer_t* rebuildTimer = nullptr;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void todoListCreateWidgets(lv_obj_t* parent, void* userData);

/** Releases widget-tracking state. Call once, after the window has been torn down. */
void todoListTeardown(Context* ctx);
