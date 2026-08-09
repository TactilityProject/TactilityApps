#pragma once
#include "TestViewBase.h"
#include <UnitPaHub.h>
#include <Unit8Encoder.h>

struct Context;

struct TestUnit8Encoder {
    Context*     app_                            = nullptr;
    UnitPaHub    hub_;
    Unit8Encoder enc_;
    lv_obj_t*    lblStatus_                     = nullptr;
    lv_obj_t*    lblCounters_[8]                = {};
    lv_obj_t*    dotButtons_[8]                 = {};
    lv_obj_t*    lblSwitch_                     = nullptr;
    lv_obj_t*    dotSwitch_                     = nullptr;
    lv_timer_t*  timer_                         = nullptr;
    int32_t      counters_[8]                   = {};
    uint32_t     ledColors_[Unit8Encoder::LED_COUNT] = {};
    bool         usingPaHub_                    = false;
};

void testUnit8EncoderStart(TestUnit8Encoder* self, lv_obj_t* parent, Context* app);
void testUnit8EncoderStop(TestUnit8Encoder* self);
