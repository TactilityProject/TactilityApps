#pragma once

#include "View.h"

#include <Tactility/kernel/Kernel.h>
#include <lvgl.h>
#include <tt_lvgl.h>

class MainView final : public View {

private:

    lv_obj_t* parent = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* ipLabel = nullptr;
    lv_obj_t* wifiButton = nullptr;
    lv_obj_t* wifiCard = nullptr;
    lv_obj_t* logTextarea = nullptr;
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* infoPanel = nullptr;

    static void wifiConnectCallback(lv_event_t* e);

public:

    void onStart(lv_obj_t* parent);
    void onStop() override;

    void updateInfoPanel(const char* ip, const char* status, lv_palette_t color);
    void logToScreen(const char* message);
    void showWifiPrompt();
    void clearLog();

    bool hasValidLogArea() const { return logTextarea != nullptr; }
};
