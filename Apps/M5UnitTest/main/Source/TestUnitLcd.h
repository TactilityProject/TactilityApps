#pragma once
#include "TestViewBase.h"
#include <UnitLcd.h>
#include <UnitPaHub.h>

class TestUnitLcd final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
    UnitPaHub hub_;
    UnitLcd   lcd_;
    lv_obj_t* lblStatus_   = nullptr;
    lv_obj_t* sliderBr_    = nullptr;
    lv_obj_t* lblRotation_ = nullptr;
    uint8_t   rotation_    = 0;
    bool      usingPaHub_  = false;
    uint8_t   lcdChannel_  = 0;

    void selectIfNeeded();

    static void onBrightnessChanged(lv_event_t* e);
    static void onRotateClicked(lv_event_t* e);
    static void onFillRedClicked(lv_event_t* e);
    static void onFillBlueClicked(lv_event_t* e);
    static void onWriteTextClicked(lv_event_t* e);
};
