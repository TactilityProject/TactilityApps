#pragma once
#include "TestViewBase.h"
#include <UnitPaHub.h>

struct Context;

struct TestUnitPaHub {
    static constexpr int CH_COUNT = UnitPaHub::NUM_CHANNELS;

    Context*    app_             = nullptr;
    UnitPaHub   hub_;
    lv_obj_t*   lblStatus_        = nullptr;
    lv_obj_t*   btnCh_[CH_COUNT]  = {};
    lv_obj_t*   lblCh_[CH_COUNT]  = {};
    lv_timer_t* timer_            = nullptr;
    int         selChannel_       = -1;
};

void testUnitPaHubStart(TestUnitPaHub* self, lv_obj_t* parent, Context* app);
void testUnitPaHubStop(TestUnitPaHub* self);
