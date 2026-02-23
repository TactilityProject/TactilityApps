/**
 * @file PatternGame.h
 * @brief Simon Says mini-game for TamaTac Play action
 */
#pragma once

#include <lvgl.h>
#include <cstdint>

class TamaTac;

class PatternGame {
private:
    TamaTac* app = nullptr;

    // UI elements
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* buttons[4] = {};
    lv_timer_t* sequenceTimer = nullptr;
    lv_timer_t* delayTimer = nullptr;

    // Game constants
    static constexpr int MAX_PATTERN = 8;
    static constexpr int START_LENGTH = 3;

    // Button colors
    static constexpr uint32_t BRIGHT_COLORS[4] = {0xFF4444, 0x4488FF, 0x44DD44, 0xFFDD44};
    static constexpr uint32_t DIM_COLORS[4] = {0x661818, 0x182860, 0x186018, 0x605818};

    // Game state
    uint8_t pattern[MAX_PATTERN] = {};
    int patternLength = START_LENGTH;
    int showIndex = 0;
    bool showPhase = false;
    int inputIndex = 0;
    int round = 0;
    bool acceptingInput = false;

    // Static callbacks
    static void onButtonClicked(lv_event_t* e);
    static void onSequenceTick(lv_timer_t* timer);
    static void onDelayDone(lv_timer_t* timer);

    // Game logic
    void generatePattern();
    void startRound();
    void beginSequenceDisplay();
    void startInputPhase();
    void handleInput(int buttonIndex);
    void roundWin();
    void gameWin();
    void gameLose();
    void returnToMain(bool won);

    // Visual helpers
    void highlightButton(int index);
    void dimButton(int index);
    void dimAllButtons();

    // Timer helpers
    void clearTimers();
    void scheduleDelay(uint32_t ms);

    // Delay action tracking
    enum class DelayAction { StartSequence, NextRound, EndGame };
    DelayAction pendingAction = DelayAction::StartSequence;
    bool gameWon = false;

public:
    static constexpr int MAX_ROUNDS = 3;

    void onStart(lv_obj_t* parent, TamaTac* appInstance);
    void onStop();
};
