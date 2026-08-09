#pragma once

#include "ConnectView.h"
#include "ConsoleView.h"

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    lv_obj_t* disconnectButton = nullptr;
    lv_obj_t* wrapperWidget = nullptr;

    enum class ActiveView { None, Connect, Console } activeView = ActiveView::None;
    ConnectViewState connectView;
    ConsoleViewState consoleView;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void serialConsoleCreateWidgets(lv_obj_t* parent, void* userData);

/** Stops whatever view is active and releases widget-tracking state. Call once, after the
 *  window has been torn down. */
void serialConsoleTeardown(Context* ctx);

/** Switches to the console view for @a dev (already opened by ConnectView before calling this). */
void showConsoleView(Context* app, Device* dev);

/** Switches back to the connect form (also disconnects the UART, via consoleViewStop). */
void showConnectView(Context* app);
