#pragma once
#include "TestViewBase.h"
#include <UnitLcd.h>
#include <UnitPaHub.h>

struct Context;

struct TestUnitLcd {
    Context*  app_        = nullptr;
    UnitPaHub hub_;
    UnitLcd   lcd_;
    lv_obj_t* lblStatus_   = nullptr;
    lv_obj_t* sliderBr_    = nullptr;
    lv_obj_t* lblRotation_ = nullptr;
    uint8_t   rotation_    = 0;
    bool      usingPaHub_  = false;
    uint8_t   lcdChannel_  = UnitPaHub::NO_CHANNEL;
};

void testUnitLcdStart(TestUnitLcd* self, lv_obj_t* parent, Context* app);
void testUnitLcdStop(TestUnitLcd* self);
