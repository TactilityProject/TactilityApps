#pragma once

#include <array>
#include <lvgl.h>
#include <tactility/lvgl_icon_shared.h>
#include <tt_app.h>

class M5UnitTest;

class TestListView {
public:
    void onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app);
    void onStop();

private:
    lv_obj_t* list_ = nullptr;
    M5UnitTest*  app_ = nullptr;

    static constexpr std::array<const char*, 11> UNIT_NAMES = {{
        "8Encoder",
        "ByteButton",
        "Joystick2",
        "Scroll",
        "PaHub",
        "Color LCD",
        "LCD Gfx Test",
        "Dual-Button",
        "CardKB2",
        "MIDI / Synth",
        "RFID 2",
    }};
    // Interface icons from shared Material icon font
    static constexpr std::array<const char*, 11> UNIT_ICONS = {{
        LVGL_ICON_SHARED_SETTINGS,       // 8Encoder      - I2C
        LVGL_ICON_SHARED_SETTINGS,       // ByteButton    - I2C
        LVGL_ICON_SHARED_GAMEPAD,        // Joystick2     - I2C
        LVGL_ICON_SHARED_SETTINGS,       // Scroll        - I2C
        LVGL_ICON_SHARED_HUB,            // PaHub         - I2C
        LVGL_ICON_SHARED_DEVICES,        // Color LCD     - I2C
        LVGL_ICON_SHARED_AREA_CHART,     // LCD Gfx Test  - I2C
        LVGL_ICON_SHARED_ELECTRIC_BOLT,  // Dual-Button   - GPIO
        LVGL_ICON_SHARED_KEYBOARD_ALT,   // CardKB2       - I2C
        LVGL_ICON_SHARED_MUSIC_NOTE,     // MIDI / Synth  - UART
        LVGL_ICON_SHARED_WIFI,           // RFID 2        - I2C
    }};
    static constexpr int UNIT_COUNT = UNIT_NAMES.size();

    static void onBtnClicked(lv_event_t* e);
};
