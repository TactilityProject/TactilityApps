#pragma once

#include <Tactility/Thread.h>

#include <lvgl.h>
#include <memory>
#include <stdint.h>

struct Context {
    uint32_t appInstanceId;

    lv_obj_t* spinbox = nullptr;
    lv_obj_t* resultLabel = nullptr;
    std::unique_ptr<tt::Thread> jobThread = nullptr;
    uint32_t wordCount = 5;
    // Instance id of the currently-shown "?" AlertDialog, or 0 if none is pending.
    uint32_t pendingHelpDialogId = 0;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void dicewareCreateWidgets(lv_obj_t* parent, void* userData);

/** Joins the background word-picking job, if any. Call once the window is torn down. */
void dicewareTeardown(Context* ctx);
