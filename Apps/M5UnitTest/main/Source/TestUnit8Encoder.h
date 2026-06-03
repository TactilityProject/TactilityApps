#pragma once
#include "TestViewBase.h"
#include <UnitPaHub.h>
#include <Unit8Encoder.h>

class TestUnit8Encoder final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
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

    static void onTimer(lv_timer_t* t);
    void update();
    void selectIfNeeded();
};
