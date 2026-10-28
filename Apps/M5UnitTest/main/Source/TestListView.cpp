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

    // Build the list from neutral widgets instead of lv_list. The Material
    // theme's lv_list button style adds large margins/paddings that make the
    // 11-entry list overflow on small and large displays alike.
    lv_obj_t* list = lv_obj_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_layout(list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(list, uiPad(), 0);
    lv_obj_set_style_pad_row(list, uiRowGap(), 0);
    lv_obj_set_scrollable(list, true);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    const lv_font_t* font = lvgl_get_text_font(uiFont());
    const lv_font_t* iconFont = lvgl_get_shared_icon_default_font();
    const lv_coord_t rowH = uiH() < 200 ? 28 : 40;

    for (int i = 0; i < UNIT_COUNT; i++) {
        lv_obj_t* btn = lv_button_create(list);
        lv_obj_set_size(btn, LV_PCT(100), rowH);
        lv_obj_set_layout(btn, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(btn, 10, 0);
        lv_obj_set_style_pad_ver(btn, 2, 0);
        lv_obj_set_style_bg_color(btn, (i & 1) ? uiPanelColor() : uiBtnColor(), 0);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, onBtnClicked, LV_EVENT_CLICKED, app);

        lv_obj_t* iconLbl = lv_label_create(btn);
        lv_label_set_text(iconLbl, UNIT_ICONS[i]);
        lv_obj_set_style_text_font(iconLbl, iconFont, 0);
        lv_obj_set_style_text_color(iconLbl, uiTextColor(), 0);
        lv_obj_set_width(iconLbl, 28);
        lv_obj_set_style_text_align(iconLbl, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t* textLbl = lv_label_create(btn);
        lv_label_set_text(textLbl, UNIT_NAMES[i]);
        lv_obj_set_style_text_font(textLbl, font, 0);
        lv_obj_set_style_text_color(textLbl, uiTextColor(), 0);
        lv_obj_set_flex_grow(textLbl, 1);
    }
}
