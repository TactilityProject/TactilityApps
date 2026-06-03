#pragma once
#include "TestViewBase.h"
#include <UnitCardKB2.h>
#include <UnitPaHub.h>

class TestUnitCardKB2 final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
    UnitPaHub   hub_;
    UnitCardKB2 unit_;
    lv_timer_t* timer_      = nullptr;
    bool        usingPaHub_ = false;

    // Connection selection overlay (shown before connecting)
    lv_obj_t* connectOverlay_ = nullptr;

    // Main content (shown after connecting)
    lv_obj_t* lblHistory_  = nullptr;
    char history_[64]       = {};
    uint8_t histLen_        = 0;

    // Keyboard grid (only built on screens >= 200px wide)
    struct KeyCell {
        const char* label;
        uint8_t     matchChar;   // ASCII to highlight; 0 = not matchable
        lv_obj_t*   btn = nullptr;
        lv_obj_t*   lbl = nullptr;
    };
    static constexpr int GRID_KEY_COUNT = 52;
    KeyCell grid_[GRID_KEY_COUNT] = {};
    int     gridCount_  = 0;
    lv_obj_t* activeBtn_ = nullptr;

    lv_obj_t* parentRef_   = nullptr;
    AppHandle handleRef_   = nullptr;

    void showConnectOverlay();
    void connectI2C();
    void connectUart();
    void buildMainUI();
    void buildGrid(lv_obj_t* parent);
    void selectIfNeeded();

    static void onTimer(lv_timer_t* t);
    static void onI2CBtn(lv_event_t* e);
    static void onUartBtn(lv_event_t* e);
    void update();
};
