#include "TestListView.h"
#include "M5UnitTest.h"
#include "UiScale.h"
#include <tt_lvgl_toolbar.h>
#include <tactility/lvgl_fonts.h>

void TestListView::onStart(lv_obj_t* parent, AppHandle handle, M5UnitTest* app) {
    app_ = app;

    tt_lvgl_toolbar_create_for_app(parent, handle);

    list_ = lv_list_create(parent);
    lv_obj_set_width(list_, LV_PCT(100));
    lv_obj_set_flex_grow(list_, 1);
    lv_obj_set_style_pad_all(list_, uiPad(), 0);
    lv_obj_set_style_pad_row(list_, uiRowGap(), 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);

    const lv_font_t* font = lvgl_get_text_font(uiFont());

    for (int i = 0; i < UNIT_COUNT; i++) {
        lv_obj_t* btn = lv_list_add_button(list_, UNIT_ICONS[i], UNIT_NAMES[i]);
        lv_obj_set_user_data(btn, (void*)(intptr_t)i);
        lv_obj_add_event_cb(btn, onBtnClicked, LV_EVENT_CLICKED, this);
        // lv_list_add_button creates: child 0 = icon label, child 1 = text label
        lv_obj_t* textLbl = lv_obj_get_child(btn, 1);
        if (textLbl) lv_obj_set_style_text_font(textLbl, font, 0);
        lv_obj_t* iconLbl = lv_obj_get_child(btn, 0);
        if (iconLbl) lv_obj_set_style_text_font(iconLbl, lvgl_get_shared_icon_font(), 0);
    }
}

void TestListView::onStop() {
    list_ = nullptr;
    app_  = nullptr;
}

void TestListView::onBtnClicked(lv_event_t* e) {
    auto* self = static_cast<TestListView*>(lv_event_get_user_data(e));
    lv_obj_t* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (self && self->app_) self->app_->showTest(idx);
}
