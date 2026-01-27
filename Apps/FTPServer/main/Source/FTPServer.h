#pragma once

#include "MainView.h"
#include "SettingsView.h"
#include "View.h"

#include <TactilityCpp/App.h>

#include <lvgl.h>
#include <tt_app.h>

class FTPServer final : public App {

    lv_obj_t* wrapperWidget = nullptr;
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* settingsButton = nullptr;
    lv_obj_t* spinner = nullptr;
    lv_obj_t* connectSwitch = nullptr;
    lv_obj_t* clearLogButton = nullptr;
    lv_timer_t* ftpStartCheckTimer = nullptr;

    MainView mainView;
    SettingsView settingsView = SettingsView(
        [this]() { showMainView(); },
        [this](const char* user, const char* pass, int port) { onSettingsSaved(user, pass, port); }
    );
    View* activeView = nullptr;

    void stopActiveView();
    void showMainView();
    void showSettingsView();
    void onSettingsSaved(const char* username, const char* password, int port);
    void onSettingsButtonPressed();
    void onSwitchToggled(bool checked);
    void onClearLogButtonPressed();

    static void onSwitchToggledCallback(lv_event_t* event);
    static void onSettingsButtonCallback(lv_event_t* event);
    static void onClearLogButtonCallback(lv_event_t* event);

public:
    void checkFtpServerStarted();

    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
};
