#pragma once
#include "TestViewBase.h"
#include <UnitDualButton.h>
#include <tactility/drivers/gpio_controller.h>

class TestUnitDualButton final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
    UnitDualButton unit_;
    bool connected_         = false;

    // Config screen
    gpio_pin_t pinA_        = 0;
    gpio_pin_t pinB_        = 49;
    lv_obj_t* lblPinA_      = nullptr;
    lv_obj_t* lblPinB_      = nullptr;
    lv_obj_t* lblError_     = nullptr;

    // Test screen
    lv_obj_t* circleA_      = nullptr;
    lv_obj_t* circleB_      = nullptr;
    lv_obj_t* circleLblA_   = nullptr;
    lv_obj_t* circleLblB_   = nullptr;

    lv_timer_t* timer_      = nullptr;

    void buildConfigScreen(lv_obj_t* parent);
    void buildTestScreen(lv_obj_t* parent);
    void update();

    static void onTimer(lv_timer_t* t);
    static void onPinADown(lv_event_t* e);
    static void onPinAUp(lv_event_t* e);
    static void onPinBDown(lv_event_t* e);
    static void onPinBUp(lv_event_t* e);
    static void onConnect(lv_event_t* e);
};
