#pragma once

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>

struct Context {
    AppInstanceId appInstanceId = 0;
    // Set by main() right after window_manager_create() returns - test-unit timer callbacks
    // need it to check whether this window is still topmost before touching any widget.
    WindowId  window            = 0;
    lv_obj_t* wrapper           = nullptr;  // full-screen container, cleaned between views

    // Currently active test-unit view (heap-allocated by its create() wrapper), or nullptr
    // when the list is shown. activeTestStop() knows how to stop+delete activeTest.
    void*     activeTest        = nullptr;
    void    (*activeTestStop)(void* self) = nullptr;
    int       activeTestIndex   = -1;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void m5UnitTestCreateWidgets(lv_obj_t* parent, void* userData);

/** Stops whatever is currently shown (a test view, if any) and releases the wrapper. Call once,
 *  after the window has been torn down. */
void m5UnitTestTeardown(Context* ctx);

/** Called by the list view when the user selects a unit to test. */
void m5UnitTestShowTest(Context* ctx, int unitIndex);

/** Called by test views (via the shared Back button) to return to the list. */
void m5UnitTestShowList(Context* ctx);
