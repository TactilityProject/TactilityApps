/**
 * @file ReactionGame.h
 * @brief Reaction time mini-game for TamaTac Play action
 */
#pragma once

#include <lvgl.h>
#include <cstdint>

class TamaTac;

class ReactionGame {
private:
    TamaTac* app = nullptr;

    // UI elements
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* targetBtn = nullptr;
    lv_timer_t* delayTimer = nullptr;

    // Game constants (MAX_ROUNDS is public for external access)
    static constexpr uint32_t MIN_DELAY_MS = 1000;
    static constexpr uint32_t MAX_DELAY_MS = 3500;
    static constexpr uint32_t GOOD_TIME_MS = 400;
    static constexpr uint32_t GREAT_TIME_MS = 250;

    // State machine
    enum class Phase { WaitForTarget, TargetShown, RoundResult, Done };
    Phase phase = Phase::WaitForTarget;

    // Game state
    int round = 0;
    int score = 0;               // 0-3 based on reaction quality
    uint32_t targetShowTime = 0; // When target appeared (millis)

    // Static callbacks
    static void onTargetClicked(lv_event_t* e);
    static void onAreaClicked(lv_event_t* e);
    static void onTimerTick(lv_timer_t* timer);

    // Game logic
    void startRound();
    void showTarget();
    void handleTap();
    void handleEarlyTap();
    void showFinalResult();
    void returnToMain();

    // Timer helpers
    void clearTimers();
    void scheduleTimer(uint32_t ms);

public:
    static constexpr int MAX_ROUNDS = 3;

    void onStart(lv_obj_t* parent, TamaTac* appInstance);
    void onStop();
};
