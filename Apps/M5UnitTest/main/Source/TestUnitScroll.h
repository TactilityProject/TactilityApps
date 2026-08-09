#pragma once
#include "TestViewBase.h"
#include <UnitScroll.h>
#include <UnitPaHub.h>

struct Context;

struct TestUnitScroll {
    Context*    app_        = nullptr;
    UnitPaHub   hub_;
    UnitScroll  unit_;
    lv_obj_t*   lblCounter_  = nullptr;
    lv_obj_t*   lblButton_   = nullptr;
    lv_obj_t*   lblLed_      = nullptr;
    lv_obj_t*   sliderR_     = nullptr;
    lv_obj_t*   sliderG_     = nullptr;
    lv_obj_t*   sliderB_     = nullptr;
    lv_timer_t* timer_       = nullptr;
    int32_t     counter_     = 0;
    bool        usingPaHub_  = false;
};

void testUnitScrollStart(TestUnitScroll* self, lv_obj_t* parent, Context* app);
void testUnitScrollStop(TestUnitScroll* self);
