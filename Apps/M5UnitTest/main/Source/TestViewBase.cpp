#include "TestViewBase.h"
#include "M5UnitTest.h"
#include "UiScale.h"
#include <tt_lvgl_toolbar.h>
#include <tactility/lvgl_fonts.h>

lv_obj_t* TestViewBase::createToolbar(lv_obj_t* parent, AppHandle handle, const char* title) {
    lv_obj_t* toolbar = tt_lvgl_toolbar_create_for_app(parent, handle);
    tt_lvgl_toolbar_set_title(toolbar, title);
    tt_lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_LEFT, onBackClicked, this);
    return toolbar;
}

void TestViewBase::createBanner(lv_obj_t* parent, const char* unitName,
                                const char* ifaceBadge, lv_color_t accentColor) {
    lv_coord_t bannerH = uiH() < 200 ? 16 : 22;

    lv_obj_t* banner = lv_obj_create(parent);
    lv_obj_set_width(banner, LV_PCT(100));
    lv_obj_set_height(banner, bannerH);
    lv_obj_set_style_bg_color(banner, accentColor, 0);
    lv_obj_set_style_bg_opa(banner, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(banner, 0, 0);
    lv_obj_set_style_radius(banner, 0, 0);
    lv_obj_set_style_pad_hor(banner, 6, 0);
    lv_obj_set_style_pad_ver(banner, 0, 0);
    lv_obj_set_layout(banner, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(banner, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(banner, LV_OBJ_FLAG_SCROLLABLE);

    // Unit name label (left-aligned)
    lv_obj_t* nameLabel = lv_label_create(banner);
    lv_label_set_text(nameLabel, unitName);
    lv_obj_set_style_text_color(nameLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(nameLabel, lvgl_get_text_font(uiFont()), 0);
    lv_obj_set_width(nameLabel, LV_SIZE_CONTENT);

    // Interface badge pill (right-aligned)
    lv_obj_t* pill = lv_obj_create(banner);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, bannerH - 4);
    lv_obj_set_style_bg_color(pill, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_30, 0);
    lv_obj_set_style_border_width(pill, 1, 0);
    lv_obj_set_style_border_color(pill, lv_color_white(), 0);
    lv_obj_set_style_border_opa(pill, LV_OPA_60, 0);
    lv_obj_set_style_radius(pill, 4, 0);
    lv_obj_set_style_pad_hor(pill, 4, 0);
    lv_obj_set_style_pad_ver(pill, 0, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* badgeLabel = lv_label_create(pill);
    lv_label_set_text(badgeLabel, ifaceBadge);
    lv_obj_set_style_text_color(badgeLabel, lv_color_white(), 0);
    lv_obj_set_style_text_font(badgeLabel, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_center(badgeLabel);
}

void TestViewBase::onBackClicked(lv_event_t* e) {
    auto* self = static_cast<TestViewBase*>(lv_event_get_user_data(e));
    if (!self || !self->app_) return;
    self->onStop();
    M5UnitTest* app = self->app_;
    app->clearActiveTestView();
    delete self;
    app->showList();
}
