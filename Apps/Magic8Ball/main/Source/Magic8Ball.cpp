#include "Magic8Ball.h"
#include <lvgl/fonts.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/device.h>
#include <tactility/drivers/keyboard.h>
#include <stdlib.h>
#include <time.h>

namespace {

/* ── Responses ─────────────────────────────────────────────────────── */

const char* responses[] = {
    /* Affirmative (10) */
    "It is certain.",
    "It is decidedly so.",
    "Without a doubt.",
    "Yes, definitely.",
    "You may rely on it.",
    "As I see it, yes.",
    "Most likely.",
    "Outlook good.",
    "Yes.",
    "Signs point to yes.",
    /* Non-committal (5) */
    "Reply hazy, try again.",
    "Ask again later.",
    "Better not tell you now.",
    "Cannot predict now.",
    "Concentrate and ask again.",
    /* Negative (5) */
    "Don't count on it.",
    "My reply is no.",
    "My sources say no.",
    "Outlook not so good.",
    "Very doubtful.",
};

#define NUM_RESPONSES (sizeof(responses) / sizeof(responses[0]))

const char* getInputHint() {
    if (device_has_active_by_type(&KEYBOARD_TYPE)) {
        return "Touch or Space to ask  Q to exit";
    }
    return "Touch the ball to ask";
}

void resizeBall(lv_obj_t* container, Context* ctx) {
    const int32_t availableWidth = lv_obj_get_content_width(container);
    const int32_t availableHeight = lv_obj_get_content_height(container);
    const int32_t hintHeight = lv_obj_get_height(ctx->hintLabel);
    const int32_t diameterByHeight = availableHeight - hintHeight - 12;
    const int32_t diameter = availableWidth < diameterByHeight ? availableWidth : diameterByHeight;
    if (diameter <= 0) return;

    lv_obj_set_size(ctx->ballObj, diameter, diameter);
    lv_obj_set_style_border_width(ctx->ballObj, diameter / 48 > 2 ? diameter / 48 : 2, 0);
    lv_obj_set_style_border_width(ctx->windowObj, diameter / 64 > 1 ? diameter / 64 : 1, 0);

    const auto fontSize = diameter >= 200 ? FONT_SIZE_LARGE
                         : diameter >= 128 ? FONT_SIZE_DEFAULT
                                           : FONT_SIZE_SMALL;
    lv_obj_set_style_text_font(ctx->answerLabel, lvgl_get_text_font(fontSize), 0);
}

void onContainerSizeChanged(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    resizeBall(lv_event_get_target_obj(e), ctx);
}

/* ── Behavior ─────────────────────────────────────────────────────── */

void revealAnswer(Context* ctx) {
    if (!ctx->seeded) {
        srand((unsigned)time(NULL));
        ctx->seeded = true;
    }

    int idx;
    do {
        idx = rand() % NUM_RESPONSES;
    } while (idx == ctx->lastIdx && NUM_RESPONSES > 1);
    ctx->lastIdx = idx;

    lv_label_set_text(ctx->answerLabel, responses[idx]);
    lv_label_set_text(ctx->hintLabel, getInputHint());
}

void onBallClick(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    revealAnswer(ctx);
}

void onKey(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    uint32_t key = lv_event_get_key(e);
    switch (key) {
        case LV_KEY_ENTER:
        case ' ':
            revealAnswer(ctx);
            break;
        case LV_KEY_ESC:
        case 'q':
        case 'Q': {
            lv_group_t* grp = lv_group_get_default();
            if (grp) lv_group_set_editing(grp, false);
            lv_group_remove_obj(lv_event_get_current_target_obj(e));
            break;
        }
        default:
            break;
    }
}

} // namespace

/* ── Lifecycle ────────────────────────────────────────────────────── */

void magic8BallCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    /* Toolbar */
    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Magic 8-Ball");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    /* Main container */
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align_to(cont, toolbar, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(cont, 12, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);
    lv_obj_set_scrollable(cont, false);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont, 0, 0);

    /* Black outer sphere */
    ctx->ballObj = lv_obj_create(cont);
    lv_obj_set_size(ctx->ballObj, 120, 120);
    lv_obj_set_style_radius(ctx->ballObj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ctx->ballObj, lv_color_hex(0x090D16), 0);
    lv_obj_set_style_bg_opa(ctx->ballObj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ctx->ballObj, lv_color_hex(0x394252), 0);
    lv_obj_set_style_border_width(ctx->ballObj, 3, 0);
    lv_obj_set_style_shadow_color(ctx->ballObj, lv_color_hex(0x000000), 0);
    lv_obj_set_style_shadow_width(ctx->ballObj, 8, 0);
    lv_obj_set_style_pad_all(ctx->ballObj, 0, 0);
    lv_obj_set_scrollable(ctx->ballObj, false);
    lv_obj_set_flex_flow(ctx->ballObj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctx->ballObj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Recessed blue answer window */
    ctx->windowObj = lv_obj_create(ctx->ballObj);
    lv_obj_set_size(ctx->windowObj, LV_PCT(78), LV_PCT(78));
    lv_obj_set_style_radius(ctx->windowObj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ctx->windowObj, lv_color_hex(0x08234A), 0);
    lv_obj_set_style_bg_opa(ctx->windowObj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ctx->windowObj, lv_color_hex(0x287AC0), 0);
    lv_obj_set_style_border_width(ctx->windowObj, 2, 0);
    lv_obj_set_style_pad_all(ctx->windowObj, 0, 0);
    lv_obj_set_scrollable(ctx->windowObj, false);
    lv_obj_set_flex_flow(ctx->windowObj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ctx->windowObj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_gesture_bubble(ctx->windowObj, true);

    /* Answer text inside the window */
    ctx->answerLabel = lv_label_create(ctx->windowObj);
    lv_label_set_text(ctx->answerLabel, "8");
    lv_obj_set_style_text_color(ctx->answerLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(ctx->answerLabel, lvgl_get_text_font(FONT_SIZE_LARGE), 0);
    lv_obj_set_style_text_align(ctx->answerLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ctx->answerLabel, LV_PCT(86));
    lv_label_set_long_mode(ctx->answerLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_gesture_bubble(ctx->answerLabel, true);

    /* Hint text below the ball */
    ctx->hintLabel = lv_label_create(cont);
    lv_label_set_text(ctx->hintLabel, getInputHint());
    lv_obj_set_style_text_color(ctx->hintLabel, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_set_style_text_font(ctx->hintLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_align(ctx->hintLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(ctx->hintLabel, LV_PCT(100));

    /* Make the ball tappable / also space */
    lv_obj_set_clickable(ctx->ballObj, true);
    lv_obj_add_event_cb(ctx->ballObj, onBallClick, LV_EVENT_CLICKED, ctx);

    lv_obj_update_layout(cont);
    resizeBall(cont, ctx);
    lv_obj_add_event_cb(cont, onContainerSizeChanged, LV_EVENT_SIZE_CHANGED, ctx);

    /* Keyboard support - no editing mode needed, just focus the ball */
    if (device_has_active_by_type(&KEYBOARD_TYPE)) {
        lv_group_t* grp = lv_group_get_default();
        if (grp) {
            lv_group_add_obj(grp, ctx->ballObj);
            lv_group_focus_obj(ctx->ballObj);
        }
        lv_obj_add_event_cb(ctx->ballObj, onKey, LV_EVENT_KEY, ctx);
    }
}

void magic8BallTeardown(Context* ctx) {
    // Don't touch ctx->ballObj here: by the time this runs, window_manager_remove() has already
    // deleted the widget tree, and LVGL detaches a deleted object from its group automatically
    // as part of that deletion (see lvgl-module keyboard.cpp's comment on obj_delete_core()'s
    // ordering) - calling lv_group_remove_obj() on it here would be a use-after-free.
    ctx->answerLabel = nullptr;
    ctx->hintLabel = nullptr;
    ctx->ballObj = nullptr;
    ctx->windowObj = nullptr;
}
