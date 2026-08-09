#include "TestListView.h"
#include "M5UnitTest.h"
#include "UiScale.h"
#include <lvgl/widgets/toolbar.h>
#include <lvgl/icons/shared.h>
#include <lvgl/fonts.h>
#include <array>

namespace {

constexpr std::array<const char*, 11> UNIT_NAMES = {{
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
constexpr std::array<const char*, 11> UNIT_ICONS = {{
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
constexpr int UNIT_COUNT = UNIT_NAMES.size();

void onBtnClicked(lv_event_t* e) {
    auto* app = static_cast<Context*>(lv_event_get_user_data(e));
    lv_obj_t* btn = lv_event_get_target_obj(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (app) m5UnitTestShowTest(app, idx);
}

} // namespace

void testListViewCreate(lv_obj_t* parent, Context* app) {
    lvgl_toolbar_create(parent, "M5 Unit Test");

    lv_obj_t* list = lv_list_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_pad_all(list, uiPad(), 0);
    lv_obj_set_style_pad_row(list, uiRowGap(), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);

    const lv_font_t* font = lvgl_get_text_font(uiFont());

    for (int i = 0; i < UNIT_COUNT; i++) {
        lv_obj_t* btn = lv_list_add_button(list, UNIT_ICONS[i], UNIT_NAMES[i]);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, onBtnClicked, LV_EVENT_CLICKED, app);
        // lv_list_add_button creates: child 0 = icon label, child 1 = text label
        lv_obj_t* textLbl = lv_obj_get_child(btn, 1);
        if (textLbl) lv_obj_set_style_text_font(textLbl, font, 0);
        lv_obj_t* iconLbl = lv_obj_get_child(btn, 0);
        if (iconLbl) lv_obj_set_style_text_font(iconLbl, lvgl_get_shared_icon_font(), 0);
    }
}
