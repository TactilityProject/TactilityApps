/**
 * @file TamaTac.h
 * @brief TamaTac virtual pet app for Tactility
 */
#pragma once

#include <app/instance.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/timers.h>

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

// Active view type
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

struct Context {
    AppInstanceId appInstanceId = 0;
    WindowId window = 0;

    // UI elements
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* wrapperWidget = nullptr;
    lv_obj_t* menuButton = nullptr;

    // Views
    MainViewState mainView;
    StatsViewState statsView;
    MenuViewState menuView;
    SettingsViewState settingsView;
    PatternGameState patternGame;
    ReactionGameState reactionGame;
    CemeteryViewState cemeteryView;
    AchievementsViewState achievementsView;
    ViewType activeView = ViewType::None;

    // Pet simulation - session-scoped: survives widget rebuilds (window burial/resurface)
    PetLogic petLogic;
    LifeStage lastKnownStage = LifeStage::Egg;
    DecaySpeed decaySpeed = DecaySpeed::Normal;
    uint32_t lastPetTime = 0;
    bool pendingResetUI = false;  // Set by main.cpp's AlertDialog result handler, consumed by MainView's anim timer

    // Session-scoped resources - created once in tamaTacInit(), released in tamaTacTeardown()
    SfxEngine* sfxEngine = nullptr;
    TimerHandle_t updateTimer = nullptr;
    SemaphoreHandle_t timerMutex = nullptr;

    // Dialog launch id for tracking the reset-confirmation AlertDialog
    uint32_t resetDialogId = 0;
};

/** Sets up state that must exist for the whole app instance lifetime: loads pet state, starts
 *  the SFX engine, and starts the 30s pet-update timer. Call once, before window_manager_create(). */
void tamaTacInit(Context* ctx);

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void tamaTacCreateWidgets(lv_obj_t* parent, void* userData);

/** Stops the update timer and SFX engine, saves pet state. Call once, after the window has been
 *  torn down. */
void tamaTacTeardown(Context* ctx);

// Action handlers (called by MainView)
void tamaTacHandleFeedAction(Context* ctx);
void tamaTacHandlePlayAction(Context* ctx);
void tamaTacHandleMedicineAction(Context* ctx);
void tamaTacHandleSleepAction(Context* ctx);
void tamaTacHandlePetTap(Context* ctx);

// Mini-game callbacks
void tamaTacOnPatternGameComplete(Context* ctx, int roundsCompleted, bool won);
void tamaTacOnReactionGameComplete(Context* ctx, int score, bool won);

// Settings handlers (called by SettingsView)
void tamaTacSetSoundEnabled(Context* ctx, bool enabled);
void tamaTacSetDecaySpeed(Context* ctx, DecaySpeed speed);

// View navigation (called by MenuView, MainView, and internally)
void showMainView(Context* ctx);
void showMenuView(Context* ctx);
void showStatsView(Context* ctx);
void showSettingsView(Context* ctx);
void showCemeteryView(Context* ctx);
void showAchievementsView(Context* ctx);
void showPatternGame(Context* ctx);
void showReactionGame(Context* ctx);
