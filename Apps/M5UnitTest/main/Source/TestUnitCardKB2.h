#pragma once
#include "TestViewBase.h"
#include <UnitCardKB2.h>
#include <UnitPaHub.h>

struct Context;

struct TestUnitCardKB2 {
    static constexpr int GRID_KEY_COUNT = 52;

    struct KeyCell {
        const char* label;
        uint8_t     matchChar;   // ASCII to highlight; 0 = not matchable
        lv_obj_t*   btn = nullptr;
        lv_obj_t*   lbl = nullptr;
    };

    Context*    app_        = nullptr;
    UnitPaHub   hub_;
    UnitCardKB2 unit_;
    lv_timer_t* timer_      = nullptr;
    bool        usingPaHub_ = false;

    // Connection selection overlay (shown before connecting)
    lv_obj_t* connectOverlay_ = nullptr;

    // Main content (shown after connecting)
    lv_obj_t* lblHistory_  = nullptr;
    char history_[64]       = {};
    uint8_t histLen_        = 0;

    // Keyboard grid (only built on screens >= 200px wide)
    KeyCell grid_[GRID_KEY_COUNT] = {};
    int     gridCount_  = 0;
    lv_obj_t* activeBtn_ = nullptr;

    lv_obj_t* parentRef_   = nullptr;
};

void testUnitCardKB2Start(TestUnitCardKB2* self, lv_obj_t* parent, Context* app);
void testUnitCardKB2Stop(TestUnitCardKB2* self);
