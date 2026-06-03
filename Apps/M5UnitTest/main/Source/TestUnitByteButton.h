#pragma once
#include "TestViewBase.h"
#include <UnitByteButton.h>
#include <UnitPaHub.h>

class TestUnitByteButton final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
    static constexpr int      BTN_COUNT       = UnitByteButton::BUTTON_COUNT;
    static constexpr uint32_t COLOR_OFF       = 0x001100;
    static constexpr uint32_t COLOR_ON        = 0x00FF44;
    static constexpr uint32_t COLOR_ERROR     = 0x440000;
    static constexpr uint32_t COLOR_PRESSED   = 0xFFFF00;

    UnitPaHub      hub_;
    UnitByteButton unit_;
    lv_obj_t*   indicators_[BTN_COUNT] = {};
    lv_timer_t* timer_                 = nullptr;
    uint32_t    ledColors_[BTN_COUNT]  = {};
    bool        prevPressed_[BTN_COUNT]= {};
    bool        usingPaHub_            = false;

    void selectIfNeeded();
    static void onTimer(lv_timer_t* t);
    void update();
};
