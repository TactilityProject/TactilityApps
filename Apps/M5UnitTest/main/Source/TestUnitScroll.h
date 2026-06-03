#pragma once
#include "TestViewBase.h"
#include <UnitScroll.h>
#include <UnitPaHub.h>

class TestUnitScroll final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
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

    void selectIfNeeded();
    static void onTimer(lv_timer_t* t);
    static void onSliderChanged(lv_event_t* e);
    void update();
    void updateLedFromSliders();
};
