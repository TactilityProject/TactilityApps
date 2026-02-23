/**
 * @file ReactionGame.cpp
 * @brief Reaction time mini-game implementation
 */

#include "ReactionGame.h"
#include "TamaTac.h"
#include "SfxEngine.h"
#include <Tactility/kernel/Kernel.h>
#include <cstdlib>
#include <cstdio>

void ReactionGame::onStart(lv_obj_t* parent, TamaTac* appInstance) {
    app = appInstance;

    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);

    int pad = isSmall ? 4 : (isXLarge ? 16 : 8);
    int targetSize = isSmall ? 80 : (isXLarge ? 200 : 120);

    // Main wrapper (tappable for early-tap detection)
    lv_obj_t* wrapper = lv_obj_create(parent);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(wrapper, pad, 0);
    lv_obj_set_style_bg_opa(wrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrapper, 0, 0);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wrapper, pad, 0);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wrapper, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wrapper, onAreaClicked, LV_EVENT_CLICKED, this);

    // Status label
    statusLabel = lv_label_create(wrapper);
    lv_label_set_text(statusLabel, "Get ready...");
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);

    // Target button (hidden initially)
    targetBtn = lv_btn_create(wrapper);
    lv_obj_set_size(targetBtn, targetSize, targetSize);
    lv_obj_set_style_bg_color(targetBtn, lv_color_hex(0x44DD44), 0);
    lv_obj_set_style_radius(targetBtn, targetSize / 2, 0);
    lv_obj_set_style_border_width(targetBtn, 0, 0);
    lv_obj_set_style_shadow_width(targetBtn, 0, 0);
    lv_obj_add_flag(targetBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(targetBtn, onTargetClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* tapLabel = lv_label_create(targetBtn);
    lv_label_set_text(tapLabel, "TAP!");
    lv_obj_center(tapLabel);

    // Initialize game
    srand(static_cast<unsigned>(tt::kernel::getMillis()));
    round = 0;
    score = 0;
    phase = Phase::WaitForTarget;
    startRound();
}

void ReactionGame::onStop() {
    clearTimers();
    statusLabel = nullptr;
    targetBtn = nullptr;
    app = nullptr;
}

void ReactionGame::clearTimers() {
    if (delayTimer) {
        lv_timer_del(delayTimer);
        delayTimer = nullptr;
    }
}

void ReactionGame::scheduleTimer(uint32_t ms) {
    clearTimers();
    delayTimer = lv_timer_create(onTimerTick, ms, this);
    lv_timer_set_repeat_count(delayTimer, 1);
}

void ReactionGame::startRound() {
    phase = Phase::WaitForTarget;
    if (targetBtn) lv_obj_add_flag(targetBtn, LV_OBJ_FLAG_HIDDEN);

    char msg[48];
    snprintf(msg, sizeof(msg), "Round %d/%d - Wait...", round + 1, MAX_ROUNDS);
    if (statusLabel) lv_label_set_text(statusLabel, msg);

    // Random delay before target appears
    uint32_t delay = MIN_DELAY_MS + (rand() % (MAX_DELAY_MS - MIN_DELAY_MS));
    scheduleTimer(delay);
}

void ReactionGame::showTarget() {
    phase = Phase::TargetShown;
    targetShowTime = tt::kernel::getMillis();

    if (targetBtn) lv_obj_clear_flag(targetBtn, LV_OBJ_FLAG_HIDDEN);
    if (statusLabel) lv_label_set_text(statusLabel, "TAP NOW!");

    SfxEngine* se = TamaTac::getSfxEngine();
    if (se) se->play(SfxId::Blip);

    // Timeout if player doesn't tap within 3 seconds
    scheduleTimer(3000);
}

void ReactionGame::handleTap() {
    uint32_t reactionTime = tt::kernel::getMillis() - targetShowTime;
    if (targetBtn) lv_obj_add_flag(targetBtn, LV_OBJ_FLAG_HIDDEN);

    const char* rating;
    if (reactionTime <= GREAT_TIME_MS) {
        score++;
        rating = "GREAT";
    } else if (reactionTime <= GOOD_TIME_MS) {
        score++;
        rating = "Good";
    } else {
        rating = "Slow";
    }

    SfxEngine* se = TamaTac::getSfxEngine();
    if (se) se->play(reactionTime <= GOOD_TIME_MS ? SfxId::Confirm : SfxId::Blip);

    char msg[64];
    snprintf(msg, sizeof(msg), "%s! %ldms", rating, (long)reactionTime);
    if (statusLabel) lv_label_set_text(statusLabel, msg);

    round++;
    phase = Phase::RoundResult;
    scheduleTimer(1500);
}

void ReactionGame::handleEarlyTap() {
    clearTimers();
    if (statusLabel) lv_label_set_text(statusLabel, "Too early!");

    SfxEngine* se = TamaTac::getSfxEngine();
    if (se) se->play(SfxId::Error);

    round++;
    phase = Phase::RoundResult;
    scheduleTimer(1500);
}

void ReactionGame::showFinalResult() {
    char msg[64];
    if (score >= MAX_ROUNDS) {
        snprintf(msg, sizeof(msg), LV_SYMBOL_OK " Perfect! %d/%d", score, MAX_ROUNDS);
    } else if (score > 0) {
        snprintf(msg, sizeof(msg), "Score: %d/%d", score, MAX_ROUNDS);
    } else {
        snprintf(msg, sizeof(msg), LV_SYMBOL_CLOSE " Score: 0/%d", MAX_ROUNDS);
    }
    if (statusLabel) lv_label_set_text(statusLabel, msg);

    SfxEngine* se = TamaTac::getSfxEngine();
    if (se) se->play(score > 0 ? SfxId::Success : SfxId::Error);

    phase = Phase::Done;
    scheduleTimer(1500);
}

void ReactionGame::returnToMain() {
    if (app) {
        app->onReactionGameComplete(score, score >= MAX_ROUNDS);
    }
}

//==============================================================================
// Timer Callback
//==============================================================================

void ReactionGame::onTimerTick(lv_timer_t* timer) {
    auto* self = static_cast<ReactionGame*>(lv_timer_get_user_data(timer));
    self->delayTimer = nullptr;

    switch (self->phase) {
        case Phase::WaitForTarget:
            self->showTarget();
            break;
        case Phase::TargetShown:
            // Timeout — player didn't tap in time
            if (self->targetBtn) lv_obj_add_flag(self->targetBtn, LV_OBJ_FLAG_HIDDEN);
            if (self->statusLabel) lv_label_set_text(self->statusLabel, "Too slow!");
            self->round++;
            self->phase = Phase::RoundResult;
            self->scheduleTimer(1500);
            break;
        case Phase::RoundResult:
            if (self->round >= MAX_ROUNDS) {
                self->showFinalResult();
            } else {
                self->startRound();
            }
            break;
        case Phase::Done:
            self->returnToMain();
            break;
    }
}

//==============================================================================
// Input Handling
//==============================================================================

void ReactionGame::onTargetClicked(lv_event_t* e) {
    auto* self = static_cast<ReactionGame*>(lv_event_get_user_data(e));
    if (self->phase == Phase::TargetShown) {
        self->handleTap();
    }
}

void ReactionGame::onAreaClicked(lv_event_t* e) {
    auto* self = static_cast<ReactionGame*>(lv_event_get_user_data(e));
    if (self->phase == Phase::WaitForTarget) {
        self->handleEarlyTap();
    }
}
