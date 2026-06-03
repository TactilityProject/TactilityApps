#include "SerialConsole.h"
#include <tt_lvgl_toolbar.h>

constexpr auto* TAG = "SerialMonitor";

void SerialConsole::stopActiveView() {
    if (activeView != nullptr) {
        activeView->onStop();
        lv_obj_clean(wrapperWidget);
        activeView = nullptr;
    }
}

void SerialConsole::showConsoleView(Device* dev) {
    if (dev == nullptr) {
        ESP_LOGE(TAG, "showConsoleView: null device");
        return;
    }
    stopActiveView();
    activeView = &consoleView;
    consoleView.onStart(wrapperWidget, dev);
    lv_obj_remove_flag(disconnectButton, LV_OBJ_FLAG_HIDDEN);
}

void SerialConsole::showConnectView() {
    stopActiveView();
    activeView = &connectView;
    connectView.onStart(wrapperWidget);
    lv_obj_add_flag(disconnectButton, LV_OBJ_FLAG_HIDDEN);
}

void SerialConsole::onDisconnect() {
    // Changing views (calling ConsoleView::stop()) also disconnects the UART
    showConnectView();
}

void SerialConsole::onDisconnectPressed(lv_event_t* event) {
    auto* app = static_cast<SerialConsole*>(lv_event_get_user_data(event));
    app->onDisconnect();
}

void SerialConsole::onShow(AppHandle appHandle, lv_obj_t* parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    auto* toolbar = tt_lvgl_toolbar_create_for_app(parent, appHandle);

    disconnectButton = tt_lvgl_toolbar_add_image_button_action(toolbar, LV_SYMBOL_POWER, onDisconnectPressed, this);
    lv_obj_add_flag(disconnectButton, LV_OBJ_FLAG_HIDDEN);

    wrapperWidget = lv_obj_create(parent);
    lv_obj_set_width(wrapperWidget, LV_PCT(100));
    lv_obj_set_flex_grow(wrapperWidget, 1);
    lv_obj_set_flex_flow(wrapperWidget, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(wrapperWidget, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(wrapperWidget, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(wrapperWidget, 0, LV_STATE_DEFAULT);

    showConnectView();
}

void SerialConsole::onHide(AppHandle context) {
    stopActiveView();
}
