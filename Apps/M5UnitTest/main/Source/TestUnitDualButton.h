#pragma once
#include "TestViewBase.h"
#include <UnitDualButton.h>
#include <tactility/drivers/gpio_controller.h>

struct Context;

struct TestUnitDualButton {
    Context* app_           = nullptr;
    UnitDualButton unit_;
    bool connected_         = false;

    // Config screen
    gpio_pin_t pinA_        = 0;
    gpio_pin_t pinB_        = 49;
    lv_obj_t* lblPinA_      = nullptr;
    lv_obj_t* lblPinB_      = nullptr;
    lv_obj_t* lblError_     = nullptr;

    // Test screen
    lv_obj_t* circleA_      = nullptr;
    lv_obj_t* circleB_      = nullptr;
    lv_obj_t* circleLblA_   = nullptr;
    lv_obj_t* circleLblB_   = nullptr;

    lv_timer_t* timer_      = nullptr;
};

void testUnitDualButtonStart(TestUnitDualButton* self, lv_obj_t* parent, Context* app);
void testUnitDualButtonStop(TestUnitDualButton* self);
