/**
 * @file PatternGame.cpp
 * @brief Simon Says mini-game implementation
 */

#include "PatternGame.h"
#include "TamaTac.h"
#include "SfxEngine.h"
#include <lvgl_window_manager/window_manager.h>
#include <cstdlib>
#include <cstdio>

namespace {

// Button colors
constexpr uint32_t BRIGHT_COLORS[4] = {0xFF4444, 0x4488FF, 0x44DD44, 0xFFDD44};
constexpr uint32_t DIM_COLORS[4] = {0x661818, 0x182860, 0x186018, 0x605818};

void highlightButton(PatternGameState* state, int index) {
    if (index >= 0 && index < 4 && state->buttons[index]) {
        lv_obj_set_style_bg_color(state->buttons[index], lv_color_hex(BRIGHT_COLORS[index]), 0);
    }
}

void dimButton(PatternGameState* state, int index) {
    if (index >= 0 && index < 4 && state->buttons[index]) {
        lv_obj_set_style_bg_color(state->buttons[index], lv_color_hex(DIM_COLORS[index]), 0);
    }
}

void dimAllButtons(PatternGameState* state) {
    for (int i = 0; i < 4; i++) {
        dimButton(state, i);
    }
}

void clearTimers(PatternGameState* state) {
    if (state->sequenceTimer) {
        lv_timer_del(state->sequenceTimer);
        state->sequenceTimer = nullptr;
    }
    if (state->delayTimer) {
        lv_timer_del(state->delayTimer);
        state->delayTimer = nullptr;
    }
}

void generatePattern(PatternGameState* state) {
    for (int i = 0; i < 8; i++) {
        state->pattern[i] = rand() % 4;
    }
}

void scheduleDelay(Context* ctx, uint32_t ms);
void startRound(Context* ctx);
void beginSequenceDisplay(Context* ctx);
void startInputPhase(PatternGameState* state);
void returnToMain(Context* ctx, bool won);

void onSequenceTick(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    PatternGameState* state = &ctx->patternGame;

    // Widgets only exist while this window is topmost - skip otherwise (window_manager deletes
    // a buried window's widgets, but this independent lv_timer_t keeps firing regardless; same
    // reasoning as GPIO.cpp's periodic status timer).
    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) return;

    if (state->showPhase) {
        // Was highlighting -> dim it
        dimButton(state, state->pattern[state->showIndex]);
        state->showIndex++;
        state->showPhase = false;

        // Check if all shown
        if (state->showIndex >= state->patternLength) {
            lv_timer_del(state->sequenceTimer);
            state->sequenceTimer = nullptr;
            startInputPhase(state);
            return;
        }

        // Short gap before next highlight
        lv_timer_set_period(state->sequenceTimer, 200);
    } else {
        // Gap done -> highlight next button
        highlightButton(state, state->pattern[state->showIndex]);
        state->showPhase = true;

        // Play blip for each flash
        if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Blip);

        // Longer highlight duration
        lv_timer_set_period(state->sequenceTimer, 400);
    }
}

void onDelayDone(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    PatternGameState* state = &ctx->patternGame;
    state->delayTimer = nullptr;

    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) return;

    switch (state->pendingAction) {
        case PatternGameState::DelayAction::StartSequence:
            beginSequenceDisplay(ctx);
            break;
        case PatternGameState::DelayAction::NextRound:
            startRound(ctx);
            break;
        case PatternGameState::DelayAction::EndGame:
            returnToMain(ctx, state->gameWon);
            break;
    }
}

void scheduleDelay(Context* ctx, uint32_t ms) {
    PatternGameState* state = &ctx->patternGame;
    if (state->delayTimer) {
        lv_timer_del(state->delayTimer);
        state->delayTimer = nullptr;
    }
    state->delayTimer = lv_timer_create(onDelayDone, ms, ctx);
    lv_timer_set_repeat_count(state->delayTimer, 1);
}

void startRound(Context* ctx) {
    PatternGameState* state = &ctx->patternGame;
    state->acceptingInput = false;
    dimAllButtons(state);

    char msg[32];
    snprintf(msg, sizeof(msg), "Round %d - Watch!", state->round + 1);
    if (state->statusLabel) lv_label_set_text(state->statusLabel, msg);

    // Delay before showing pattern
    state->pendingAction = PatternGameState::DelayAction::StartSequence;
    scheduleDelay(ctx, 800);
}

void beginSequenceDisplay(Context* ctx) {
    PatternGameState* state = &ctx->patternGame;
    state->showIndex = 0;
    state->showPhase = false;
    state->sequenceTimer = lv_timer_create(onSequenceTick, 350, ctx);
}

void startInputPhase(PatternGameState* state) {
    state->acceptingInput = true;
    state->inputIndex = 0;
    if (state->statusLabel) lv_label_set_text(state->statusLabel, "Your turn!");
}

void returnToMain(Context* ctx, bool won) {
    tamaTacOnPatternGameComplete(ctx, ctx->patternGame.round, won);
}

void gameWin(Context* ctx) {
    PatternGameState* state = &ctx->patternGame;
    if (state->statusLabel) lv_label_set_text(state->statusLabel, LV_SYMBOL_OK " You win!");
    dimAllButtons(state);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Success);

    state->gameWon = true;
    state->pendingAction = PatternGameState::DelayAction::EndGame;
    scheduleDelay(ctx, 1500);
}

void gameLose(Context* ctx) {
    PatternGameState* state = &ctx->patternGame;
    char msg[48];
    snprintf(msg, sizeof(msg), LV_SYMBOL_CLOSE " Wrong! Rounds: %d/%d", state->round, PATTERN_GAME_MAX_ROUNDS);
    if (state->statusLabel) lv_label_set_text(state->statusLabel, msg);
    dimAllButtons(state);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Error);

    state->gameWon = false;
    state->pendingAction = PatternGameState::DelayAction::EndGame;
    scheduleDelay(ctx, 1500);
}

void roundWin(Context* ctx) {
    PatternGameState* state = &ctx->patternGame;
    state->round++;

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Confirm);

    if (state->round >= PATTERN_GAME_MAX_ROUNDS) {
        gameWin(ctx);
        return;
    }

    // Show success message, then start next round
    char msg[48];
    snprintf(msg, sizeof(msg), "Correct! Round %d next...", state->round + 1);
    if (state->statusLabel) lv_label_set_text(state->statusLabel, msg);

    state->patternLength++;
    if (state->patternLength > 8) state->patternLength = 8;

    state->pendingAction = PatternGameState::DelayAction::NextRound;
    scheduleDelay(ctx, 1200);
}

void handleInput(Context* ctx, int buttonIndex) {
    PatternGameState* state = &ctx->patternGame;

    if (buttonIndex == state->pattern[state->inputIndex]) {
        // Correct!
        if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Blip);
        state->inputIndex++;

        if (state->inputIndex >= state->patternLength) {
            // Completed the full pattern
            state->acceptingInput = false;
            roundWin(ctx);
        }
    } else {
        // Wrong!
        state->acceptingInput = false;
        gameLose(ctx);
    }
}

void onButtonClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    PatternGameState* state = &ctx->patternGame;
    if (!state->acceptingInput) return;

    lv_obj_t* target = lv_event_get_target_obj(e);
    for (int i = 0; i < 4; i++) {
        if (target == state->buttons[i]) {
            handleInput(ctx, i);
            return;
        }
    }
}

} // namespace

void patternGameCreateWidgets(lv_obj_t* parent, Context* ctx) {
    PatternGameState* state = &ctx->patternGame;

    // Screen size detection
    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);
    bool isLarge = !isXLarge && (screenWidth >= 400 && screenHeight >= 300);

    // Scaled dimensions
    int btnSize = isSmall ? 50 : (isXLarge ? 120 : (isLarge ? 90 : 70));
    int gap = isSmall ? 4 : (isXLarge ? 12 : (isLarge ? 8 : 6));
    int pad = isSmall ? 4 : (isXLarge ? 16 : 8);
    int radius = isSmall ? 6 : (isXLarge ? 16 : 10);

    // Main wrapper
    lv_obj_t* wrapper = lv_obj_create(parent);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(wrapper, pad, 0);
    lv_obj_set_style_bg_opa(wrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wrapper, 0, 0);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wrapper, gap, 0);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Status label
    state->statusLabel = lv_label_create(wrapper);
    lv_label_set_text(state->statusLabel, "Get ready...");
    lv_obj_set_style_text_align(state->statusLabel, LV_TEXT_ALIGN_CENTER, 0);

    // 2x2 button grid
    lv_obj_t* grid = lv_obj_create(wrapper);
    int gridSize = btnSize * 2 + gap * 3;
    lv_obj_set_size(grid, gridSize, gridSize);
    lv_obj_set_style_pad_all(grid, gap, 0);
    lv_obj_set_style_pad_row(grid, gap, 0);
    lv_obj_set_style_pad_column(grid, gap, 0);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_radius(grid, radius, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    // Create 4 game buttons
    for (int i = 0; i < 4; i++) {
        state->buttons[i] = lv_btn_create(grid);
        lv_obj_set_size(state->buttons[i], btnSize, btnSize);
        lv_obj_set_style_bg_color(state->buttons[i], lv_color_hex(DIM_COLORS[i]), 0);
        lv_obj_set_style_bg_color(state->buttons[i], lv_color_hex(BRIGHT_COLORS[i]), LV_STATE_PRESSED);
        lv_obj_set_style_radius(state->buttons[i], radius, 0);
        lv_obj_set_style_border_width(state->buttons[i], 0, 0);
        lv_obj_set_style_shadow_width(state->buttons[i], 0, 0);
        lv_obj_add_event_cb(state->buttons[i], onButtonClicked, LV_EVENT_CLICKED, ctx);
    }

    // Initialize game
    state->round = 0;
    state->patternLength = 3;
    state->acceptingInput = false;
    generatePattern(state);
    startRound(ctx);
}

void patternGameStop(Context* ctx) {
    PatternGameState* state = &ctx->patternGame;
    clearTimers(state);
    state->statusLabel = nullptr;
    for (int i = 0; i < 4; i++) state->buttons[i] = nullptr;
}
