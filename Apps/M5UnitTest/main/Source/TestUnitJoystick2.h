#pragma once
#include "TestViewBase.h"
#include <UnitJoystick2.h>
#include <UnitPaHub.h>

class TestUnitJoystick2 final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
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

    void selectIfNeeded();
    static void onTimer(lv_timer_t* t);
    void update();
};
