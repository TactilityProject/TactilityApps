#pragma once
#include "TestViewBase.h"
#include <UnitLcd.h>
#include <UnitPaHub.h>

// PDQ graphics benchmark test, matching the Arduino_GFX PDQgraphicstest sketch.
// Phases run sequentially via one-shot LVGL timers; results appear in the LVGL
// log and are also drawn back onto the LCD unit at the end.
class TestUnitLcdGfx final : public TestViewBase {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) override;
    void onStop() override;

private:
    UnitPaHub  hub_;
    UnitLcd    lcd_;
    bool       usingPaHub_ = false;

    lv_obj_t*   lblPhase_  = nullptr;
    lv_obj_t*   lblLog_    = nullptr;
    lv_timer_t* timer_     = nullptr;

    static constexpr int RESULT_COUNT = 15;

    int      phase_    = 0;
    char     logBuf_[768] = {};
    uint32_t results_[RESULT_COUNT] = {};

    // Pre-computed layout constants (set in onStart after lcd_.begin)
    int16_t w_ = 0, h_ = 0;
    int16_t minDim_ = 0, minDim1_ = 0;       // min(w,h) and min(w,h)-1
    int16_t cx_ = 0, cy_ = 0, cx1_ = 0, cy1_ = 0;
    int16_t cMin_ = 0, cMin1_ = 0;           // min(cx1,cy1) and min(cx1,cy1)-1

    void selectIfNeeded();
    void runNextPhase();
    void appendLog(const char* name, uint32_t us);
    void drawResultsOnLcd();

    static void onTimer(lv_timer_t* t);

    // Benchmark phases - matching PDQ order exactly
    uint32_t testFillScreen();
    uint32_t testText();
    uint32_t testPixels();
    uint32_t testLines();
    uint32_t testFastLines();
    uint32_t testFilledRects();
    uint32_t testRects();
    uint32_t testFilledTriangles();
    uint32_t testTriangles();
    uint32_t testFilledCircles();
    uint32_t testCircles();
    uint32_t testFillArcs();
    uint32_t testArcs();
    uint32_t testFilledRoundRects();
    uint32_t testRoundRects();
};
