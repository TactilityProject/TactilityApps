#pragma once

#include <Tactility/RecursiveMutex.h>
#include <Tactility/Timer.h>

#include <lvgl.h>
#include <memory>
#include <vector>

struct Context {
    uint32_t appInstanceId;
    // Set by main() right after window_manager_create() returns - the timer callback needs it
    // to check whether this window is still topmost before touching any widget (see
    // gpioOnTimer()'s comment).
    uint32_t window = 0;

    std::vector<lv_obj_t*> pinWidgets;
    std::vector<bool> pinStates;
    // Constructed once in gpioInit(); needs ctx's address for its callback closure, so it can't
    // be a plain default member initializer (Context doesn't exist yet at that point in main()).
    std::unique_ptr<tt::Timer> timer;
    tt::RecursiveMutex mutex;
};

/** Sets up state that must exist for the whole app instance lifetime. Call once, right after
 *  constructing the Context and before window_manager_create(). */
void gpioInit(Context* ctx);

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void gpioCreateWidgets(lv_obj_t* parent, void* userData);

/** Stops the periodic refresh timer and releases widget-tracking state. Call once, after the
 *  window has been torn down. */
void gpioTeardown(Context* ctx);
