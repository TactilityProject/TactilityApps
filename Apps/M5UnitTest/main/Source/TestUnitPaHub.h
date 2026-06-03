#pragma once
#include "TestViewBase.h"
#include <UnitPaHub.h>

class TestUnitPaHub final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
    static constexpr int CH_COUNT = UnitPaHub::NUM_CHANNELS;

    UnitPaHub   hub_;
    lv_obj_t*   lblStatus_        = nullptr;
    lv_obj_t*   btnCh_[CH_COUNT]  = {};
    lv_obj_t*   lblCh_[CH_COUNT]  = {};
    lv_timer_t* timer_            = nullptr;
    int         selChannel_       = -1;

    static void onChannelBtn(lv_event_t* e);
    static void onTimer(lv_timer_t* t);
    void probeSelected();
};
