#include "M5UnitTest.h"
#include "TestListView.h"
#include "TestUnit8Encoder.h"
#include "TestUnitByteButton.h"
#include "TestUnitJoystick2.h"
#include "TestUnitScroll.h"
#include "TestUnitPaHub.h"
#include "TestUnitLcd.h"
#include "TestUnitLcdGfx.h"
#include "TestUnitDualButton.h"
#include "TestUnitCardKB2.h"
#include "TestUnitMidi.h"
#include "TestUnitRfid2.h"

#include <esp_log.h>

constexpr auto* TAG = "M5UnitTest";

namespace {

struct UnitEntry {
    void* (*create)(lv_obj_t* parent, Context* app);
    void  (*stop)(void* self);
};

template<typename T, void (*Start)(T*, lv_obj_t*, Context*), void (*Stop)(T*)>
void* createUnit(lv_obj_t* parent, Context* app) {
    auto* self = new T();
    Start(self, parent, app);
    return self;
}

template<typename T, void (*Stop)(T*)>
void stopUnit(void* p) {
    auto* self = static_cast<T*>(p);
    Stop(self);
    delete self;
}

constexpr UnitEntry UNIT_ENTRIES[11] = {
    { createUnit<TestUnit8Encoder,   testUnit8EncoderStart,   testUnit8EncoderStop>,   stopUnit<TestUnit8Encoder,   testUnit8EncoderStop> },
    { createUnit<TestUnitByteButton, testUnitByteButtonStart, testUnitByteButtonStop>, stopUnit<TestUnitByteButton, testUnitByteButtonStop> },
    { createUnit<TestUnitJoystick2,  testUnitJoystick2Start,  testUnitJoystick2Stop>,  stopUnit<TestUnitJoystick2,  testUnitJoystick2Stop> },
    { createUnit<TestUnitScroll,     testUnitScrollStart,     testUnitScrollStop>,     stopUnit<TestUnitScroll,     testUnitScrollStop> },
    { createUnit<TestUnitPaHub,      testUnitPaHubStart,      testUnitPaHubStop>,      stopUnit<TestUnitPaHub,      testUnitPaHubStop> },
    { createUnit<TestUnitLcd,        testUnitLcdStart,        testUnitLcdStop>,        stopUnit<TestUnitLcd,        testUnitLcdStop> },
    { createUnit<TestUnitLcdGfx,     testUnitLcdGfxStart,     testUnitLcdGfxStop>,     stopUnit<TestUnitLcdGfx,     testUnitLcdGfxStop> },
    { createUnit<TestUnitDualButton, testUnitDualButtonStart, testUnitDualButtonStop>, stopUnit<TestUnitDualButton, testUnitDualButtonStop> },
    { createUnit<TestUnitCardKB2,    testUnitCardKB2Start,    testUnitCardKB2Stop>,    stopUnit<TestUnitCardKB2,    testUnitCardKB2Stop> },
    { createUnit<TestUnitMidi,       testUnitMidiStart,       testUnitMidiStop>,       stopUnit<TestUnitMidi,       testUnitMidiStop> },
    { createUnit<TestUnitRfid2,      testUnitRfid2Start,      testUnitRfid2Stop>,      stopUnit<TestUnitRfid2,      testUnitRfid2Stop> },
};
constexpr int UNIT_COUNT = 11;

void stopActiveTest(Context* ctx) {
    if (ctx->activeTest && ctx->activeTestStop) {
        ctx->activeTestStop(ctx->activeTest);
    }
    ctx->activeTest = nullptr;
    ctx->activeTestStop = nullptr;
    ctx->activeTestIndex = -1;
}

} // namespace

void m5UnitTestCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    ctx->wrapper = lv_obj_create(parent);
    lv_obj_set_width(ctx->wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->wrapper, 1);
    lv_obj_set_layout(ctx->wrapper, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(ctx->wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctx->wrapper, 0, 0);
    lv_obj_set_style_bg_opa(ctx->wrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctx->wrapper, 0, 0);

    // Resurfacing after being buried (e.g. another app was started on top): rebuild whichever
    // test was active. The C++ state object itself already existed if so, but its widgets were
    // just destroyed along with the rest of the window's tree, so we drop it and start fresh
    // (matches this framework's create_widgets contract used for every other app).
    if (ctx->activeTestIndex >= 0) {
        int index = ctx->activeTestIndex;
        stopActiveTest(ctx);
        m5UnitTestShowTest(ctx, index);
    } else {
        testListViewCreate(ctx->wrapper, ctx);
    }
}

void m5UnitTestTeardown(Context* ctx) {
    stopActiveTest(ctx);
    ctx->wrapper = nullptr;
}

void m5UnitTestShowTest(Context* ctx, int unitIndex) {
    stopActiveTest(ctx);

    if (unitIndex < 0 || unitIndex >= UNIT_COUNT) {
        m5UnitTestShowList(ctx);
        return;
    }

    lv_obj_clean(ctx->wrapper);
    ESP_LOGI(TAG, "Opening test for unit %d", unitIndex);

    const UnitEntry& entry = UNIT_ENTRIES[unitIndex];
    ctx->activeTest = entry.create(ctx->wrapper, ctx);
    ctx->activeTestStop = entry.stop;
    ctx->activeTestIndex = unitIndex;
}

void m5UnitTestShowList(Context* ctx) {
    stopActiveTest(ctx);
    lv_obj_clean(ctx->wrapper);
    testListViewCreate(ctx->wrapper, ctx);
}
