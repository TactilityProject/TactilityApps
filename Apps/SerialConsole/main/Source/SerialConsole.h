#pragma once

#include "ConnectView.h"
#include "ConsoleView.h"

#include <TactilityCpp/App.h>

class SerialConsole final : public App {

    lv_obj_t* disconnectButton = nullptr;
    lv_obj_t* wrapperWidget = nullptr;
    ConnectView connectView = ConnectView([this](Device* dev){
        showConsoleView(dev);
    });
    ConsoleView consoleView;
    View* activeView = nullptr;

    void stopActiveView();
    void showConsoleView(Device* dev);
    void showConnectView();
    void onDisconnect();
    static void onDisconnectPressed(lv_event_t* event);

public:

    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
};
