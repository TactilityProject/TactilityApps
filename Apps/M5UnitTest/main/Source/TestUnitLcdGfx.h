#pragma once
#include "TestViewBase.h"
#include <UnitLcd.h>
#include <UnitPaHub.h>

struct Context;

// PDQ graphics benchmark test, matching the Arduino_GFX PDQgraphicstest sketch.
// Phases run sequentially via one-shot LVGL timers; results appear in the LVGL
// log and are also drawn back onto the LCD unit at the end.
struct TestUnitLcdGfx {
    static constexpr int RESULT_COUNT = 15;

    Context*   app_        = nullptr;
    UnitPaHub  hub_;
    UnitLcd    lcd_;
    bool       usingPaHub_ = false;

    lv_obj_t*   lblPhase_  = nullptr;
    lv_obj_t*   lblLog_    = nullptr;
    lv_timer_t* timer_     = nullptr;

    int      phase_    = 0;
    char     logBuf_[768] = {};
    uint32_t results_[RESULT_COUNT] = {};

    // Pre-computed layout constants (set in start after lcd_.begin)
    int16_t w_ = 0, h_ = 0;
    int16_t minDim_ = 0, minDim1_ = 0;       // min(w,h) and min(w,h)-1
    int16_t cx_ = 0, cy_ = 0, cx1_ = 0, cy1_ = 0;
    int16_t cMin_ = 0, cMin1_ = 0;           // min(cx1,cy1) and min(cx1,cy1)-1
};

void testUnitLcdGfxStart(TestUnitLcdGfx* self, lv_obj_t* parent, Context* app);
void testUnitLcdGfxStop(TestUnitLcdGfx* self);
