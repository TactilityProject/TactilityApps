#include "Magic8Ball.h"
#include <tt_lvgl_toolbar.h>
#include <tt_lvgl_keyboard.h>
#include <stdlib.h>
#include <time.h>

/* ── Responses ─────────────────────────────────────────────────────── */

static const char* responses[] = {
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

static const char* getInputHint() {
    if (tt_lvgl_hardware_keyboard_is_available()) {
        return "Touch or press Space to ask";
    }
    return "Touch the ball to ask";
}

/* ── Methods ──────────────────────────────────────────────────────── */

void Magic8Ball::revealAnswer() {
    if (!seeded) {
        srand((unsigned)time(NULL));
        seeded = true;
    }

    int idx;
    do {
        idx = rand() % NUM_RESPONSES;
    } while (idx == lastIdx && NUM_RESPONSES > 1);
    lastIdx = idx;

    lv_label_set_text(answerLabel, responses[idx]);
    lv_label_set_text(hintLabel, getInputHint());
}

void Magic8Ball::onBallClick(lv_event_t* e) {
    auto* self = (Magic8Ball*)lv_event_get_user_data(e);
    self->revealAnswer();
}

void Magic8Ball::onKey(lv_event_t* e) {
    auto* self = (Magic8Ball*)lv_event_get_user_data(e);
    uint32_t key = lv_event_get_key(e);
    if (key == LV_KEY_ENTER || key == ' ') {
        self->revealAnswer();
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

void Magic8Ball::onShow(AppHandle app, lv_obj_t* parent) {
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    /* Toolbar */
    lv_obj_t* toolbar = tt_lvgl_toolbar_create_for_app(parent, app);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    /* Main container */
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align_to(cont, toolbar, LV_ALIGN_OUT_BOTTOM_MID, 0, 0);
    lv_obj_set_style_pad_all(cont, 2, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont, 0, 0);

    /* "8" ball circle */
    ballObj = lv_obj_create(cont);
    lv_obj_set_size(ballObj, 120, 120);
    lv_obj_set_style_radius(ballObj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ballObj, lv_color_make(0x10, 0x10, 0x30), 0);
    lv_obj_set_style_bg_opa(ballObj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ballObj, lv_color_make(0x40, 0x40, 0x80), 0);
    lv_obj_set_style_border_width(ballObj, 3, 0);
    lv_obj_remove_flag(ballObj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(ballObj, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ballObj, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Answer text inside the ball */
    answerLabel = lv_label_create(ballObj);
    lv_label_set_text(answerLabel, "8");
    lv_obj_set_style_text_color(answerLabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(answerLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_align(answerLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(answerLabel, 100);
    lv_label_set_long_mode(answerLabel, LV_LABEL_LONG_WRAP);

    /* Hint text below the ball */
    hintLabel = lv_label_create(cont);
    lv_label_set_text(hintLabel, getInputHint());
    lv_obj_set_style_text_color(hintLabel, lv_color_make(0x88, 0x88, 0x88), 0);
    lv_obj_set_style_text_font(hintLabel, lv_font_get_default(), 0);

    /* Make the ball tappable / also space */
    lv_obj_add_flag(ballObj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ballObj, onBallClick, LV_EVENT_CLICKED, this);

    /* Keyboard support */
    lv_group_t* grp = lv_group_get_default();
    if (grp) lv_group_add_obj(grp, ballObj);
    lv_obj_add_event_cb(ballObj, onKey, LV_EVENT_KEY, this);
}

void Magic8Ball::onHide(AppHandle app) {
    lv_group_t* grp = lv_group_get_default();
    if (grp && ballObj) {
        lv_group_remove_obj(ballObj);
    }
    answerLabel = nullptr;
    hintLabel = nullptr;
    ballObj = nullptr;
}
