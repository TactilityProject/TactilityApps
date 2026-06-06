#include "M5UnitTest.h"
#include "TestListView.h"
#include "TestViewBase.h"
#include "TestUnit8Encoder.h"
#include "TestUnitByteButton.h"
#include "TestUnitJoystick2.h"
#include "TestUnitScroll.h"
#include "TestUnitPaHub.h"
#include "TestUnitLcd.h"
#include "TestUnitDualButton.h"
#include "TestUnitCardKB2.h"
#include "TestUnitMidi.h"
#include "TestUnitRfid2.h"
#include "TestUnitLcdGfx.h"

#include <tactility/device.h>
#include <tt_lvgl_toolbar.h>
#include <esp_log.h>

constexpr auto* TAG = "M5UnitTest";

M5UnitTest* M5UnitTest::s_instance = nullptr;

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void M5UnitTest::onShow(AppHandle handle, lv_obj_t* parent) {
    s_instance  = this;
    appHandle_  = handle;

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    createWrapper(parent);

    if (!listView_) {
        listView_ = new TestListView();
        listView_->onStart(wrapper_, handle, this);
    }
}

void M5UnitTest::onHide(AppHandle handle) {
    if (activeTestView_) {
        activeTestView_->onStop();
        delete activeTestView_;
        activeTestView_ = nullptr;
    }
    if (listView_) listView_->onStop();
    delete listView_;
    listView_   = nullptr;
    wrapper_    = nullptr;
    appHandle_  = nullptr;
    s_instance  = nullptr;
}

// ---------------------------------------------------------------------------
// View switching
// ---------------------------------------------------------------------------

template<typename T>
static TestViewBase* makeTestView(lv_obj_t* wrapper, AppHandle handle, M5UnitTest* app) {
    auto* v = new T();
    v->onStart(wrapper, handle, app);
    return v;
}

void M5UnitTest::createWrapper(lv_obj_t* parent) {
    wrapper_ = lv_obj_create(parent);
    lv_obj_set_width(wrapper_, LV_PCT(100));
    lv_obj_set_flex_grow(wrapper_, 1);
    lv_obj_set_layout(wrapper_, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(wrapper_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(wrapper_, 0, 0);
    lv_obj_set_style_bg_opa(wrapper_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrapper_, 0, 0);
}

void M5UnitTest::showTest(int unitIndex) {
    // Tear down any active test view and the list view, then clean the wrapper
    if (activeTestView_) {
        activeTestView_->onStop();
        delete activeTestView_;
        activeTestView_ = nullptr;
    }
    if (listView_) {
        listView_->onStop();
        delete listView_;
        listView_ = nullptr;
    }
    lv_obj_clean(wrapper_);

    ESP_LOGI(TAG, "Opening test for unit %d", unitIndex);

    switch (unitIndex) {
        case 0:  activeTestView_ = makeTestView<TestUnit8Encoder>  (wrapper_, appHandle_, this); break;
        case 1:  activeTestView_ = makeTestView<TestUnitByteButton>(wrapper_, appHandle_, this); break;
        case 2:  activeTestView_ = makeTestView<TestUnitJoystick2> (wrapper_, appHandle_, this); break;
        case 3:  activeTestView_ = makeTestView<TestUnitScroll>    (wrapper_, appHandle_, this); break;
        case 4:  activeTestView_ = makeTestView<TestUnitPaHub>     (wrapper_, appHandle_, this); break;
        case 5:  activeTestView_ = makeTestView<TestUnitLcd>       (wrapper_, appHandle_, this); break;
        case 6:  activeTestView_ = makeTestView<TestUnitLcdGfx>    (wrapper_, appHandle_, this); break;
        case 7:  activeTestView_ = makeTestView<TestUnitDualButton>(wrapper_, appHandle_, this); break;
        case 8:  activeTestView_ = makeTestView<TestUnitCardKB2>   (wrapper_, appHandle_, this); break;
        case 9:  activeTestView_ = makeTestView<TestUnitMidi>      (wrapper_, appHandle_, this); break;
        case 10: activeTestView_ = makeTestView<TestUnitRfid2>     (wrapper_, appHandle_, this); break;
        default: showList(); return;
    }
}

void M5UnitTest::showList() {
    if (activeTestView_) {
        activeTestView_->onStop();
        delete activeTestView_;
        activeTestView_ = nullptr;
    }
    lv_obj_clean(wrapper_);

    listView_ = new TestListView();
    listView_->onStart(wrapper_, appHandle_, this);
}
