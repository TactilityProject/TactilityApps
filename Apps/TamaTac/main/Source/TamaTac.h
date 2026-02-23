/**
 * @file TamaTac.h
 * @brief TamaTac virtual pet app for Tactility
 */
#pragma once

#include <TactilityCpp/App.h>
#include <tt_app.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "PetLogic.h"
#include "Sprites.h"
#include "MainView.h"
#include "StatsView.h"
#include "MenuView.h"
#include "SettingsView.h"
#include "PatternGame.h"
#include "ReactionGame.h"
#include "CemeteryView.h"
#include "Achievements.h"
#include "SfxEngine.h"

// Canvas buffers for sprite rendering (shared by views)
extern lv_color_t TamaTac_canvasBuffer[72 * 72];
extern lv_color_t TamaTac_iconBuffers[12][16 * 16];

class TamaTac final : public App {
public:
    // Active view type (scoped to TamaTac class)
    enum class ViewType {
        None,
        Main,
        Menu,
        Stats,
        Settings,
        PatternGameView,
        ReactionGameView,
        CemeteryViewType,
        AchievementsViewType
    };

private:
    // UI elements
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* wrapperWidget = nullptr;
    lv_obj_t* menuButton = nullptr;

    // Views
    MainView mainView;
    StatsView statsView;
    MenuView menuView;
    SettingsView settingsView;
    PatternGame patternGame;
    ReactionGame reactionGame;
    CemeteryView cemeteryView;
    AchievementsView achievementsView;
    ViewType activeView = ViewType::None;

    // Static data (singleton — only one TamaTac instance exists at a time)
    static PetLogic* petLogic;
    static TimerHandle_t updateTimer;
    static SemaphoreHandle_t timerMutex;
    static AppHandle currentApp;
    static AppLaunchId resetDialogId;
    static LifeStage lastKnownStage;
    static SfxEngine* sfxEngine;
    static DecaySpeed decaySpeed;
    static uint32_t lastPetTime;
    static bool pendingResetUI;

public:
    void onCreate(AppHandle app) override;
    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
    void onDestroy(AppHandle app) override;
    void onResult(AppHandle app, void* data, AppLaunchId launchId, AppResult result, BundleHandle resultData) override;

    // Action handlers (called by MainView)
    void handleFeedAction();
    void handlePlayAction();
    void handleMedicineAction();
    void handleSleepAction();
    void handlePetTap();

    // Mini-game callbacks
    void onPatternGameComplete(int roundsCompleted, bool won);
    void onReactionGameComplete(int score, bool won);

    // Settings handlers (called by SettingsView)
    void setSoundEnabled(bool enabled);
    void setDecaySpeed(DecaySpeed speed);

    // View navigation (called by MenuView)
    void showMainView();
    void showStatsView();
    void showSettingsView();
    void showCemeteryView();
    void showAchievementsView();

    // Getters (note: getSfxEngine() may return nullptr before onCreate() or after onDestroy())
    static DecaySpeed getDecaySpeed() { return decaySpeed; }
    static SfxEngine* getSfxEngine() { return sfxEngine; }

private:
    // View management
    void stopActiveView();
    void showMenuView();

    // Timer callback (called every 30 seconds)
    static void onTimerUpdate(TimerHandle_t timer);

    // Event handlers
    static void onCleanClicked(lv_event_t* e);
    static void onMenuClicked(lv_event_t* e);
    static void onResetClicked(lv_event_t* e);

    // View navigation (internal)
    void showPatternGame();
    void showReactionGame();

    friend class MainView;
    friend class StatsView;
    friend class MenuView;
    friend class SettingsView;
    friend class PatternGame;
    friend class ReactionGame;
    friend class CemeteryView;
    friend class AchievementsView;
};
