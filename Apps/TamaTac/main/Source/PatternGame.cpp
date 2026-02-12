/**
 * @file PatternGame.cpp
 * @brief Simon Says mini-game implementation
 */

#include "PatternGame.h"
#include "TamaTac.h"
#include "SfxEngine.h"
#include <cstdlib>
#include <cstdio>

void PatternGame::onStart(lv_obj_t* parent, TamaTac* appInstance) {
    app = appInstance;

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
    statusLabel = lv_label_create(wrapper);
    lv_label_set_text(statusLabel, "Get ready...");
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);

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
        buttons[i] = lv_btn_create(grid);
        lv_obj_set_size(buttons[i], btnSize, btnSize);
        lv_obj_set_style_bg_color(buttons[i], lv_color_hex(DIM_COLORS[i]), 0);
        lv_obj_set_style_bg_color(buttons[i], lv_color_hex(BRIGHT_COLORS[i]), LV_STATE_PRESSED);
        lv_obj_set_style_radius(buttons[i], radius, 0);
        lv_obj_set_style_border_width(buttons[i], 0, 0);
        lv_obj_set_style_shadow_width(buttons[i], 0, 0);
        lv_obj_add_event_cb(buttons[i], onButtonClicked, LV_EVENT_CLICKED, this);
    }

    // Initialize game
    round = 0;
    patternLength = START_LENGTH;
    acceptingInput = false;
    generatePattern();
    startRound();
}

void PatternGame::onStop() {
    clearTimers();
    statusLabel = nullptr;
    for (int i = 0; i < 4; i++) buttons[i] = nullptr;
    app = nullptr;
}

void PatternGame::clearTimers() {
    if (sequenceTimer) {
        lv_timer_del(sequenceTimer);
        sequenceTimer = nullptr;
    }
    if (delayTimer) {
        lv_timer_del(delayTimer);
        delayTimer = nullptr;
    }
}

void PatternGame::generatePattern() {
    for (int i = 0; i < MAX_PATTERN; i++) {
        pattern[i] = rand() % 4;
    }
}

void PatternGame::startRound() {
    acceptingInput = false;
    dimAllButtons();

    char msg[32];
    snprintf(msg, sizeof(msg), "Round %d - Watch!", round + 1);
    if (statusLabel) lv_label_set_text(statusLabel, msg);

    // Delay before showing pattern
    pendingAction = DelayAction::StartSequence;
    scheduleDelay(800);
}

void PatternGame::beginSequenceDisplay() {
    showIndex = 0;
    showPhase = false;
    sequenceTimer = lv_timer_create(onSequenceTick, 350, this);
}

void PatternGame::startInputPhase() {
    acceptingInput = true;
    inputIndex = 0;
    if (statusLabel) lv_label_set_text(statusLabel, "Your turn!");
}

void PatternGame::scheduleDelay(uint32_t ms) {
    if (delayTimer) {
        lv_timer_del(delayTimer);
        delayTimer = nullptr;
    }
    delayTimer = lv_timer_create(onDelayDone, ms, this);
    lv_timer_set_repeat_count(delayTimer, 1);
}

//==============================================================================
// Timer Callbacks
//==============================================================================

void PatternGame::onSequenceTick(lv_timer_t* timer) {
    auto* self = static_cast<PatternGame*>(lv_timer_get_user_data(timer));

    if (self->showPhase) {
        // Was highlighting → dim it
        self->dimButton(self->pattern[self->showIndex]);
        self->showIndex++;
        self->showPhase = false;

        // Check if all shown
        if (self->showIndex >= self->patternLength) {
            lv_timer_del(self->sequenceTimer);
            self->sequenceTimer = nullptr;
            self->startInputPhase();
            return;
        }

        // Short gap before next highlight
        lv_timer_set_period(self->sequenceTimer, 200);
    } else {
        // Gap done → highlight next button
        self->highlightButton(self->pattern[self->showIndex]);
        self->showPhase = true;

        // Play blip for each flash
        SfxEngine* se = TamaTac::getSfxEngine();
        if (se) se->play(SfxId::Blip);

        // Longer highlight duration
        lv_timer_set_period(self->sequenceTimer, 400);
    }
}

void PatternGame::onDelayDone(lv_timer_t* timer) {
    auto* self = static_cast<PatternGame*>(lv_timer_get_user_data(timer));
    self->delayTimer = nullptr;

    switch (self->pendingAction) {
        case DelayAction::StartSequence:
            self->beginSequenceDisplay();
            break;
        case DelayAction::NextRound:
            self->startRound();
            break;
        case DelayAction::EndGame:
            self->returnToMain(self->gameWon);
            break;
    }
}

//==============================================================================
// Input Handling
//==============================================================================

void PatternGame::onButtonClicked(lv_event_t* e) {
    auto* self = static_cast<PatternGame*>(lv_event_get_user_data(e));
    if (!self->acceptingInput) return;

    lv_obj_t* target = lv_event_get_target_obj(e);
    for (int i = 0; i < 4; i++) {
        if (target == self->buttons[i]) {
            self->handleInput(i);
            return;
        }
    }
}

void PatternGame::handleInput(int buttonIndex) {
    SfxEngine* se = TamaTac::getSfxEngine();

    if (buttonIndex == pattern[inputIndex]) {
        // Correct!
        if (se) se->play(SfxId::Blip);
        inputIndex++;

        if (inputIndex >= patternLength) {
            // Completed the full pattern
            acceptingInput = false;
            roundWin();
        }
    } else {
        // Wrong!
        acceptingInput = false;
        gameLose();
    }
}

void PatternGame::roundWin() {
    round++;

    SfxEngine* se = TamaTac::getSfxEngine();
    if (se) se->play(SfxId::Confirm);

    if (round >= MAX_ROUNDS) {
        gameWin();
        return;
    }

    // Show success message, then start next round
    char msg[48];
    snprintf(msg, sizeof(msg), "Correct! Round %d next...", round + 1);
    if (statusLabel) lv_label_set_text(statusLabel, msg);

    patternLength++;
    if (patternLength > MAX_PATTERN) patternLength = MAX_PATTERN;

    pendingAction = DelayAction::NextRound;
    scheduleDelay(1200);
}

void PatternGame::gameWin() {
    if (statusLabel) lv_label_set_text(statusLabel, LV_SYMBOL_OK " You win!");
    dimAllButtons();

    SfxEngine* se = TamaTac::getSfxEngine();
    if (se) se->play(SfxId::Success);

    gameWon = true;
    pendingAction = DelayAction::EndGame;
    scheduleDelay(1500);
}

void PatternGame::gameLose() {
    char msg[48];
    snprintf(msg, sizeof(msg), LV_SYMBOL_CLOSE " Wrong! Rounds: %d/%d", round, MAX_ROUNDS);
    if (statusLabel) lv_label_set_text(statusLabel, msg);
    dimAllButtons();

    SfxEngine* se = TamaTac::getSfxEngine();
    if (se) se->play(SfxId::Error);

    gameWon = false;
    pendingAction = DelayAction::EndGame;
    scheduleDelay(1500);
}

void PatternGame::returnToMain(bool won) {
    if (app) {
        app->onPatternGameComplete(round, won);
    }
}

//==============================================================================
// Visual Helpers
//==============================================================================

void PatternGame::highlightButton(int index) {
    if (index >= 0 && index < 4 && buttons[index]) {
        lv_obj_set_style_bg_color(buttons[index], lv_color_hex(BRIGHT_COLORS[index]), 0);
    }
}

void PatternGame::dimButton(int index) {
    if (index >= 0 && index < 4 && buttons[index]) {
        lv_obj_set_style_bg_color(buttons[index], lv_color_hex(DIM_COLORS[index]), 0);
    }
}

void PatternGame::dimAllButtons() {
    for (int i = 0; i < 4; i++) {
        dimButton(i);
    }
}
