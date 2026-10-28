#include "AirMouse.h"

#include <lvgl/lvgl.h>

namespace {

void onLeftPressed(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->buttons |= 0x01;
    sendMouseReport(ctx, 0, 0);
}

void onLeftReleased(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->buttons &= ~0x01;
    sendMouseReport(ctx, 0, 0);
}

void onRightPressed(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_PRESSED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->buttons |= 0x02;
    sendMouseReport(ctx, 0, 0);
}

void onRightReleased(lv_event_t* event) {
    if (lv_event_get_code(event) != LV_EVENT_RELEASED) return;
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->buttons &= ~0x02;
    sendMouseReport(ctx, 0, 0);
}

/**
 * One of the two click pads: it shares the row with its sibling and fills its height.
 *
 * Zeroes the button's own margin on purpose. The theme gives buttons a margin to leave room
 * for their focus ring, but flex grow does not take a growing item's own margins into account
 * when it divides the row (lv_flex.c), while placement does add them - so two grown pads push
 * the right one out of the row by 3x that margin (visible as the right pad running off the
 * display on narrow screens). The row reserves that room for both of them instead.
 */
lv_obj_t* createClickPad(lv_obj_t* parent, const char* title,
                         lv_event_cb_t onPressed, lv_event_cb_t onReleased, Context* ctx) {
    lv_obj_t* pad = lv_btn_create(parent);
    lv_obj_set_style_margin_all(pad, 0, 0);
    lv_obj_set_flex_grow(pad, 1);
    lv_obj_set_height(pad, LV_PCT(100));
    lv_obj_add_event_cb(pad, onPressed, LV_EVENT_PRESSED, ctx);
    lv_obj_add_event_cb(pad, onReleased, LV_EVENT_RELEASED, ctx);
    lv_obj_t* label = lv_label_create(pad);
    lv_label_set_text(label, title);
    lv_obj_center(label);
    return pad;
}

} // namespace

void mainViewCreate(Context* ctx) {
    lv_coord_t w = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t h = lv_display_get_vertical_resolution(nullptr);
    int gap = (w < 280 || h < 180) ? 2 : 4;

    // Dragging a pad must not scroll the window, so the content stays fixed here.
    lv_obj_set_scrollable(ctx->content, false);

    lv_obj_t* row = lv_obj_create(ctx->content);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Room for the pads' focus ring (see createClickPad) and the gap to the window edge.
    lv_obj_set_style_pad_hor(row, gap, 0);
    lv_obj_set_style_pad_column(row, gap, 0);
    lv_obj_set_scrollable(row, false);

    ctx->leftClickBtn  = createClickPad(row, "Left", onLeftPressed, onLeftReleased, ctx);
    ctx->rightClickBtn = createClickPad(row, "Right", onRightPressed, onRightReleased, ctx);
}
