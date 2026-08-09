#pragma once
#include "TestViewBase.h"
#include <UnitRfid2.h>
#include <UnitPaHub.h>

struct Context;

struct TestUnitRfid2 {
    Context*     app_        = nullptr;
    UnitPaHub    hub_;
    UnitRfid2    unit_;
    lv_timer_t*  timer_       = nullptr;
    lv_timer_t*  pulseTimer_  = nullptr;
    bool         usingPaHub_  = false;
    bool         cardShown_   = false;

    // Idle group
    lv_obj_t*    idleGroup_   = nullptr;
    lv_obj_t*    circle_      = nullptr;

    // Card info group
    lv_obj_t*    cardGroup_   = nullptr;
    lv_obj_t*    lblUid_      = nullptr;
    lv_obj_t*    lblType_     = nullptr;
    lv_obj_t*    lblSak_      = nullptr;

    // Pulse state
    uint8_t      pulseOpa_    = 220;
    int8_t       pulseDir_    = 8;

    // Stored card info
    UnitRfid2::Uid      lastUid_   = {};
    UnitRfid2::CardType cardType_  = UnitRfid2::CardType::Unknown;
};

void testUnitRfid2Start(TestUnitRfid2* self, lv_obj_t* parent, Context* app);
void testUnitRfid2Stop(TestUnitRfid2* self);
