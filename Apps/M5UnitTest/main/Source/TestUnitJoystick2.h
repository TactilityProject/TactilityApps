#pragma once
#include "TestViewBase.h"
#include <UnitJoystick2.h>
#include <UnitPaHub.h>

struct Context;

struct TestUnitJoystick2 {
    Context*      app_       = nullptr;
    UnitPaHub     hub_;
    UnitJoystick2 unit_;
    lv_obj_t*   lblXY_      = nullptr;
    lv_obj_t*   lblButton_  = nullptr;
    lv_obj_t*   dot_        = nullptr;
    lv_obj_t*   joyCont_    = nullptr;
    lv_timer_t* timer_      = nullptr;
    bool        usingPaHub_ = false;
    int         joyArea_    = 120;
    int         dotSize_    = 16;
};

void testUnitJoystick2Start(TestUnitJoystick2* self, lv_obj_t* parent, Context* app);
void testUnitJoystick2Stop(TestUnitJoystick2* self);
