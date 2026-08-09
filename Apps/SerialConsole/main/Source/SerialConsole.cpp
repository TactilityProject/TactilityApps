#include "SerialConsole.h"

#include <esp_log.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>

constexpr auto* TAG = "SerialMonitor";

namespace {

void stopActiveView(Context* ctx) {
    if (ctx->activeView == Context::ActiveView::None) return;

    if (ctx->activeView == Context::ActiveView::Connect) {
        connectViewStop(ctx);
    } else if (ctx->activeView == Context::ActiveView::Console) {
        consoleViewStop(ctx);
    }

    // wrapperWidget only exists while this window is topmost - skip cleaning it otherwise.
    // Final app teardown calls this after window_manager_remove() already deleted it (same
    // reasoning as GPIO.cpp's periodic status timer guard).
    if (window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED) {
        lv_obj_clean(ctx->wrapperWidget);
    }
    ctx->activeView = Context::ActiveView::None;
}

void onDisconnectPressed(lv_event_t* event) {
    auto* app = static_cast<Context*>(lv_event_get_user_data(event));
    // Changing views (calling consoleViewStop) also disconnects the UART
    showConnectView(app);
}

} // namespace

void showConsoleView(Context* app, Device* dev) {
    if (dev == nullptr) {
        ESP_LOGE(TAG, "showConsoleView: null device");
        return;
    }
    stopActiveView(app);
    app->activeView = Context::ActiveView::Console;
    consoleViewCreate(app->wrapperWidget, app, dev);
    lv_obj_remove_flag(app->disconnectButton, LV_OBJ_FLAG_HIDDEN);
}

void showConnectView(Context* app) {
    stopActiveView(app);
    app->activeView = Context::ActiveView::Connect;
    connectViewCreate(app->wrapperWidget, app);
    lv_obj_add_flag(app->disconnectButton, LV_OBJ_FLAG_HIDDEN);
}

void serialConsoleCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    auto* toolbar = lvgl_toolbar_create(parent, "Serial Console");

    ctx->disconnectButton = lvgl_toolbar_add_image_button_action(toolbar, LV_SYMBOL_POWER, onDisconnectPressed, ctx);

    ctx->wrapperWidget = lv_obj_create(parent);
    lv_obj_set_width(ctx->wrapperWidget, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->wrapperWidget, 1);
    lv_obj_set_flex_flow(ctx->wrapperWidget, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctx->wrapperWidget, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ctx->wrapperWidget, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ctx->wrapperWidget, 0, LV_STATE_DEFAULT);

    // Both branches below rebuild widgets directly (connectViewCreate/consoleViewRebuildWidgets)
    // instead of going through showConnectView()/showConsoleView() - those call stopActiveView()
    // first, which would try to read/save state out of widgets that window_manager has, by this
    // point, already destroyed (this callback only runs because they were destroyed - on first
    // creation there's nothing to stop anyway). Only a real view switch (user action) or final
    // teardown should ever call stopActiveView().
    if (ctx->activeView == Context::ActiveView::Console) {
        // Resurfacing after being buried while a session was active: window_manager only ever
        // destroys the widget tree, never our background threads or the open UART, so just
        // rebuild the widgets around the still-running session instead of dropping it (see
        // ConsoleView.h's comment on consoleViewRebuildWidgets()).
        lv_obj_remove_flag(ctx->disconnectButton, LV_OBJ_FLAG_HIDDEN);
        consoleViewRebuildWidgets(ctx->wrapperWidget, ctx);
    } else {
        // First creation, or resurfacing on the (stateless) connect form.
        lv_obj_add_flag(ctx->disconnectButton, LV_OBJ_FLAG_HIDDEN);
        connectViewCreate(ctx->wrapperWidget, ctx);
        ctx->activeView = Context::ActiveView::Connect;
    }
}

void serialConsoleTeardown(Context* ctx) {
    stopActiveView(ctx);
    ctx->disconnectButton = nullptr;
    ctx->wrapperWidget = nullptr;
}
