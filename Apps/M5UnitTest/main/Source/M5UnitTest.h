#pragma once

#include <tt_app.h>
#include <lvgl.h>
#include <TactilityCpp/App.h>

// Forward declarations for test views
class TestListView;
class TestViewBase;

class M5UnitTest final : public App {
public:
    void onShow(AppHandle handle, lv_obj_t* parent) override;
    void onHide(AppHandle handle) override;

    // Called by TestListView when user selects a unit to test
    void showTest(int unitIndex);
    // Called by test views when user presses back
    void showList();
    // Called by the Back button path after the view has already been deleted
    void clearActiveTestView() { activeTestView_ = nullptr; }

    AppHandle getAppHandle() const { return appHandle_; }

private:
    AppHandle     appHandle_       = nullptr;
    lv_obj_t*     wrapper_         = nullptr;  // full-screen container, cleaned between views
    TestListView* listView_        = nullptr;
    TestViewBase* activeTestView_  = nullptr;

    static M5UnitTest* s_instance;

    void createWrapper(lv_obj_t* parent);
};
