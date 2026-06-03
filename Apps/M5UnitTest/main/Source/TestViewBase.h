#pragma once

#include <tt_app.h>
#include <lvgl.h>

class M5UnitTest;

// Minimal interface shared by all test views.
// To return to the list, concrete views call showList() on the owning M5UnitTest.
// M5UnitTest calls onStop() then deletes the view after the Back button is tapped.
class TestViewBase {
public:
    virtual ~TestViewBase() = default;
    virtual void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) = 0;
    virtual void onStop() = 0;

protected:
    M5UnitTest* app_ = nullptr;

    // Accent colors matching M5Stack product image palette
    static constexpr lv_color_t COLOR_I2C  = LV_COLOR_MAKE(0x1A, 0x6E, 0xC8); // M5 blue
    static constexpr lv_color_t COLOR_GPIO = LV_COLOR_MAKE(0xC0, 0x20, 0x20); // red
    static constexpr lv_color_t COLOR_UART = LV_COLOR_MAKE(0x20, 0x90, 0x50); // green

    // Creates a standard toolbar with a Back button that returns to the list.
    lv_obj_t* createToolbar(lv_obj_t* parent, AppHandle handle, const char* title);

    // Creates a colored identity banner strip below the toolbar.
    // ifaceBadge: short string e.g. "I2C", "GPIO", "UART"
    void createBanner(lv_obj_t* parent, const char* unitName,
                      const char* ifaceBadge, lv_color_t accentColor);

    static void onBackClicked(lv_event_t* e);
};
