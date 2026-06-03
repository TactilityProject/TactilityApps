#pragma once
#include "TestViewBase.h"
#include <UnitRfid2.h>
#include <UnitPaHub.h>
#include <tt_app.h>

class TestUnitRfid2 final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
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

    void selectIfNeeded();
    void showCard(const UnitRfid2::Uid& uid);

    static void onTimer(lv_timer_t* t);
    static void onPulseTimer(lv_timer_t* t);
    static void onClear(lv_event_t* e);
};
