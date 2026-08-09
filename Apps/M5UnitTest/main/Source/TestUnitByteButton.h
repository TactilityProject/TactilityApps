#pragma once
#include "TestViewBase.h"
#include <UnitByteButton.h>
#include <UnitPaHub.h>

struct Context;

struct TestUnitByteButton {
    static constexpr int      BTN_COUNT       = UnitByteButton::BUTTON_COUNT;
    static constexpr uint32_t COLOR_OFF       = 0x001100;
    static constexpr uint32_t COLOR_ON        = 0x00FF44;
    static constexpr uint32_t COLOR_ERROR     = 0x440000;
    static constexpr uint32_t COLOR_PRESSED   = 0xFFFF00;

    Context*       app_                = nullptr;
    UnitPaHub      hub_;
    UnitByteButton unit_;
    lv_obj_t*   indicators_[BTN_COUNT] = {};
    lv_timer_t* timer_                 = nullptr;
    uint32_t    ledColors_[BTN_COUNT]  = {};
    bool        prevPressed_[BTN_COUNT]= {};
    bool        usingPaHub_            = false;
};

void testUnitByteButtonStart(TestUnitByteButton* self, lv_obj_t* parent, Context* app);
void testUnitByteButtonStop(TestUnitByteButton* self);
