/**
 * @file ReactionGame.cpp
 * @brief Reaction time mini-game implementation
 */

#include "ReactionGame.h"
#include "TamaTac.h"
#include "SfxEngine.h"
#include <lvgl_window_manager/window_manager.h>
#include <Tactility/kernel/Kernel.h>
#include <cstdlib>
#include <cstdio>

namespace {

constexpr uint32_t MIN_DELAY_MS = 1000;
constexpr uint32_t MAX_DELAY_MS = 3500;
constexpr uint32_t GOOD_TIME_MS = 400;
constexpr uint32_t GREAT_TIME_MS = 250;

void clearTimers(ReactionGameState* state) {
    if (state->delayTimer) {
        lv_timer_del(state->delayTimer);
        state->delayTimer = nullptr;
    }
}

void scheduleTimer(Context* ctx, uint32_t ms);

void startRound(Context* ctx) {
    ReactionGameState* state = &ctx->reactionGame;
    state->phase = ReactionGameState::Phase::WaitForTarget;
    if (state->targetBtn) lv_obj_add_flag(state->targetBtn, LV_OBJ_FLAG_HIDDEN);

    char msg[48];
    snprintf(msg, sizeof(msg), "Round %d/%d - Wait...", state->round + 1, REACTION_GAME_MAX_ROUNDS);
    if (state->statusLabel) lv_label_set_text(state->statusLabel, msg);

    // Random delay before target appears
    uint32_t delay = MIN_DELAY_MS + (rand() % (MAX_DELAY_MS - MIN_DELAY_MS));
    scheduleTimer(ctx, delay);
}

void showTarget(Context* ctx) {
    ReactionGameState* state = &ctx->reactionGame;
    state->phase = ReactionGameState::Phase::TargetShown;
    state->targetShowTime = tt::kernel::getMillis();

    if (state->targetBtn) lv_obj_clear_flag(state->targetBtn, LV_OBJ_FLAG_HIDDEN);
    if (state->statusLabel) lv_label_set_text(state->statusLabel, "TAP NOW!");

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Blip);

    // Timeout if player doesn't tap within 3 seconds
    scheduleTimer(ctx, 3000);
}

void returnToMain(Context* ctx) {
    ReactionGameState* state = &ctx->reactionGame;
    tamaTacOnReactionGameComplete(ctx, state->score, state->score >= REACTION_GAME_MAX_ROUNDS);
}

void showFinalResult(Context* ctx) {
    ReactionGameState* state = &ctx->reactionGame;
    char msg[64];
    if (state->score >= REACTION_GAME_MAX_ROUNDS) {
        snprintf(msg, sizeof(msg), LV_SYMBOL_OK " Perfect! %d/%d", state->score, REACTION_GAME_MAX_ROUNDS);
    } else if (state->score > 0) {
        snprintf(msg, sizeof(msg), "Score: %d/%d", state->score, REACTION_GAME_MAX_ROUNDS);
    } else {
        snprintf(msg, sizeof(msg), LV_SYMBOL_CLOSE " Score: 0/%d", REACTION_GAME_MAX_ROUNDS);
    }
    if (state->statusLabel) lv_label_set_text(state->statusLabel, msg);

    if (ctx->sfxEngine) ctx->sfxEngine->play(state->score > 0 ? SfxId::Success : SfxId::Error);

    state->phase = ReactionGameState::Phase::Done;
    scheduleTimer(ctx, 1500);
}

void handleTap(Context* ctx) {
    ReactionGameState* state = &ctx->reactionGame;
    uint32_t reactionTime = tt::kernel::getMillis() - state->targetShowTime;
    if (state->targetBtn) lv_obj_add_flag(state->targetBtn, LV_OBJ_FLAG_HIDDEN);

    const char* rating;
    if (reactionTime <= GREAT_TIME_MS) {
        state->score++;
        rating = "GREAT";
    } else if (reactionTime <= GOOD_TIME_MS) {
        state->score++;
        rating = "Good";
    } else {
        rating = "Slow";
    }

    if (ctx->sfxEngine) ctx->sfxEngine->play(reactionTime <= GOOD_TIME_MS ? SfxId::Confirm : SfxId::Blip);

    char msg[64];
    snprintf(msg, sizeof(msg), "%s! %ldms", rating, (long)reactionTime);
    if (state->statusLabel) lv_label_set_text(state->statusLabel, msg);

    state->round++;
    state->phase = ReactionGameState::Phase::RoundResult;
    scheduleTimer(ctx, 1500);
}

void handleEarlyTap(Context* ctx) {
    ReactionGameState* state = &ctx->reactionGame;
    clearTimers(state);
    if (state->statusLabel) lv_label_set_text(state->statusLabel, "Too early!");

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Error);

    state->round++;
    state->phase = ReactionGameState::Phase::RoundResult;
    scheduleTimer(ctx, 1500);
}

void onTimerTick(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    ReactionGameState* state = &ctx->reactionGame;
    state->delayTimer = nullptr;

    // Widgets only exist while this window is topmost - skip otherwise (window_manager deletes
    // a buried window's widgets, but this independent lv_timer_t keeps firing regardless; same
    // reasoning as GPIO.cpp's periodic status timer).
    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) return;

    switch (state->phase) {
        case ReactionGameState::Phase::WaitForTarget:
            showTarget(ctx);
            break;
        case ReactionGameState::Phase::TargetShown:
            // Timeout - player didn't tap in time
            if (state->targetBtn) lv_obj_add_flag(state->targetBtn, LV_OBJ_FLAG_HIDDEN);
            if (state->statusLabel) lv_label_set_text(state->statusLabel, "Too slow!");
            state->round++;
            state->phase = ReactionGameState::Phase::RoundResult;
            scheduleTimer(ctx, 1500);
            break;
        case ReactionGameState::Phase::RoundResult:
            if (state->round >= REACTION_GAME_MAX_ROUNDS) {
                showFinalResult(ctx);
            } else {
                startRound(ctx);
            }
            break;
        case ReactionGameState::Phase::Done:
            returnToMain(ctx);
            break;
    }
}

void scheduleTimer(Context* ctx, uint32_t ms) {
    clearTimers(&ctx->reactionGame);
    ctx->reactionGame.delayTimer = lv_timer_create(onTimerTick, ms, ctx);
    lv_timer_set_repeat_count(ctx->reactionGame.delayTimer, 1);
}

void onTargetClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx->reactionGame.phase == ReactionGameState::Phase::TargetShown) {
        handleTap(ctx);
    }
}

void onAreaClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx->reactionGame.phase == ReactionGameState::Phase::WaitForTarget) {
        handleEarlyTap(ctx);
    }
}

} // namespace

void reactionGameCreateWidgets(lv_obj_t* parent, Context* ctx) {
    ReactionGameState* state = &ctx->reactionGame;

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
    lv_obj_add_event_cb(wrapper, onAreaClicked, LV_EVENT_CLICKED, ctx);

    // Status label
    state->statusLabel = lv_label_create(wrapper);
    lv_label_set_text(state->statusLabel, "Get ready...");
    lv_obj_set_style_text_align(state->statusLabel, LV_TEXT_ALIGN_CENTER, 0);

    // Target button (hidden initially)
    state->targetBtn = lv_btn_create(wrapper);
    lv_obj_set_size(state->targetBtn, targetSize, targetSize);
    lv_obj_set_style_bg_color(state->targetBtn, lv_color_hex(0x44DD44), 0);
    lv_obj_set_style_radius(state->targetBtn, targetSize / 2, 0);
    lv_obj_set_style_border_width(state->targetBtn, 0, 0);
    lv_obj_set_style_shadow_width(state->targetBtn, 0, 0);
    lv_obj_add_flag(state->targetBtn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(state->targetBtn, onTargetClicked, LV_EVENT_CLICKED, ctx);

    lv_obj_t* tapLabel = lv_label_create(state->targetBtn);
    lv_label_set_text(tapLabel, "TAP!");
    lv_obj_center(tapLabel);

    // Initialize game
    srand(static_cast<unsigned>(tt::kernel::getMillis()));
    state->round = 0;
    state->score = 0;
    state->phase = ReactionGameState::Phase::WaitForTarget;
    startRound(ctx);
}

void reactionGameStop(Context* ctx) {
    ReactionGameState* state = &ctx->reactionGame;
    clearTimers(state);
    state->statusLabel = nullptr;
    state->targetBtn = nullptr;
}
