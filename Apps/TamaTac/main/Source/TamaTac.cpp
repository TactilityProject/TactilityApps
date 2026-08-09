/**
 * @file TamaTac.cpp
 * @brief TamaTac virtual pet app implementation
 */

#include "TamaTac.h"
#include "SpriteData.h"
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>
#include <app/manager.h>
#include <Tactility/kernel/Kernel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace {

constexpr auto* TAG = "TamaTac";

void onCleanClicked(lv_event_t* e);
void onMenuClicked(lv_event_t* e);
void onResetClicked(lv_event_t* e);
void onTimerUpdate(TimerHandle_t timer);

// Cleans up whatever view is currently active and, if requested, clears the wrapper widget.
// @a cleanWrapperWidget must be false when called after window_manager_remove() has already
// destroyed the widget tree (ctx->wrapperWidget is a dangling pointer at that point) - true
// otherwise (ctx->wrapperWidget was just freshly recreated by the caller). Each view's onStop()
// only deletes its own independent lv_timer_t objects and nulls its own widget pointers - it
// never dereferences them - so calling it is always safe regardless of window state.
void stopActiveView(Context* ctx, bool cleanWrapperWidget) {
    switch (ctx->activeView) {
        case ViewType::Main:
            mainViewStop(ctx);
            break;
        case ViewType::Menu:
            menuViewStop(ctx);
            break;
        case ViewType::Stats:
            statsViewStop(ctx);
            break;
        case ViewType::Settings:
            settingsViewStop(ctx);
            break;
        case ViewType::PatternGameView:
            patternGameStop(ctx);
            break;
        case ViewType::ReactionGameView:
            reactionGameStop(ctx);
            break;
        case ViewType::CemeteryViewType:
            cemeteryViewStop(ctx);
            break;
        case ViewType::AchievementsViewType:
            achievementsViewStop(ctx);
            break;
        case ViewType::None:
            break;
    }

    if (cleanWrapperWidget && ctx->wrapperWidget) {
        lv_obj_clean(ctx->wrapperWidget);
    }

    ctx->activeView = ViewType::None;
}

void onCleanClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;

    if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

    if (ctx->activeView == ViewType::Main) {
        int poopCount = ctx->petLogic.getStats().poopCount;
        if (poopCount > 0) {
            ctx->petLogic.performAction(PetAction::Clean);
            ctx->petLogic.saveState();
            mainViewUpdateUI(ctx);

            if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Clean);
            achievementsIncrementCleanCount();

            mainViewSetStatusText(ctx, "All clean!");
        } else {
            mainViewSetStatusText(ctx, "Nothing to clean!");
        }
    }

    if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);
}

void onMenuClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;

    if (ctx->activeView == ViewType::Main) {
        showMenuView(ctx);
    } else {
        showMainView(ctx);
    }
}

void onResetClicked([[maybe_unused]] lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;

    const char* argv[] = {
        "Reset Pet?",
        "This will start over with a new pet. Your current pet will be lost forever!",
        "Reset",
        "Cancel",
    };
    app_manager_start_for_result("AlertDialog", ctx->appInstanceId, 4, argv, &ctx->resetDialogId);
}

void onTimerUpdate(TimerHandle_t timer) {
    auto* ctx = static_cast<Context*>(pvTimerGetTimerID(timer));

    // Hold mutex for entire callback so tamaTacTeardown() can block until we finish
    if (ctx->timerMutex == nullptr || xSemaphoreTake(ctx->timerMutex, 0) != pdTRUE) return;

    bool wasAlive = !ctx->petLogic.isDead();
    // Capture stage before update() - checkHealth() sets stage to Ghost on death
    LifeStage stageBeforeDeath = ctx->petLogic.getStats().stage;

    uint32_t now = tt::kernel::getMillis();
    ctx->petLogic.update(now);
    ctx->petLogic.saveState();

    // Check achievements
    const PetStats& stats = ctx->petLogic.getStats();

    // Evolution achievements
    switch (stats.stage) {
        case LifeStage::Baby:  achievementsUnlock(AchievementId::ReachBaby); break;
        case LifeStage::Teen:  achievementsUnlock(AchievementId::ReachTeen); break;
        case LifeStage::Adult: achievementsUnlock(AchievementId::ReachAdult); break;
        case LifeStage::Elder: achievementsUnlock(AchievementId::ReachElder); break;
        default: break;
    }

    // Survival achievement
    if (stats.ageHours >= 24 && !stats.isDead) {
        achievementsUnlock(AchievementId::Survivor24h);
    }

    // Full stats achievement
    if (stats.hunger >= 90 && stats.happiness >= 90 && stats.health >= 90 && stats.energy >= 90) {
        achievementsUnlock(AchievementId::FullStats);
    }

    // Record death in cemetery (use pre-death stage, not Ghost)
    if (wasAlive && ctx->petLogic.isDead()) {
        cemeteryViewRecordDeath(stats.personality, stageBeforeDeath, stats.ageHours);
    }

    xSemaphoreGive(ctx->timerMutex);
}

} // namespace

// Canvas buffer definitions (shared by views via extern)
// 72x72 supports 24x24 sprites at 3x scale (medium/large/xlarge screens)
lv_color_t TamaTac_canvasBuffer[72 * 72];
lv_color_t TamaTac_iconBuffers[12][16 * 16];

void tamaTacInit(Context* ctx) {
    // Seed RNG once so rand() produces different sequences each run
    srand(tt::kernel::getMillis());

    if (!ctx->petLogic.loadState()) {
        // No save data - pet starts fresh
    }
    ctx->lastKnownStage = ctx->petLogic.getStats().stage;

    ctx->timerMutex = xSemaphoreCreateMutex();

    ctx->sfxEngine = new SfxEngine();
    ctx->sfxEngine->start();

    bool soundEnabled;
    settingsViewLoadSettings(&soundEnabled, &ctx->decaySpeed);
    ctx->sfxEngine->setEnabled(soundEnabled);
    PetLogic::setDecaySpeed(static_cast<uint8_t>(ctx->decaySpeed));

    ctx->updateTimer = xTimerCreate(
        "PetUpdate",
        pdMS_TO_TICKS(30000),
        pdTRUE,
        ctx,
        onTimerUpdate
    );
    if (ctx->updateTimer != nullptr) {
        xTimerStart(ctx->updateTimer, 0);
    }
}

void tamaTacCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    ctx->toolbar = lvgl_toolbar_create(parent, "TamaTac");

    ctx->menuButton = lvgl_toolbar_add_text_button_action(ctx->toolbar, LV_SYMBOL_LIST, onMenuClicked, ctx);
    lvgl_toolbar_add_text_button_action(ctx->toolbar, LV_SYMBOL_TRASH, onCleanClicked, ctx);
    lvgl_toolbar_add_text_button_action(ctx->toolbar, LV_SYMBOL_REFRESH, onResetClicked, ctx);

    ctx->wrapperWidget = lv_obj_create(parent);
    lv_obj_set_width(ctx->wrapperWidget, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->wrapperWidget, 1);
    lv_obj_set_style_pad_all(ctx->wrapperWidget, 0, 0);
    lv_obj_set_style_border_width(ctx->wrapperWidget, 0, 0);
    lv_obj_set_style_bg_opa(ctx->wrapperWidget, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(ctx->wrapperWidget, LV_OBJ_FLAG_SCROLLABLE);

    // Rebuild whichever view was active. On first creation this is None (-> Main). On a
    // resurface after this window was buried (e.g. by the reset AlertDialog, or by an unrelated
    // app switching to briefly), it's whatever the user had open - pet state, achievements, and
    // game-round counters all live in Context/PetLogic and survive burial fine (only the widget
    // tree was destroyed), so rebuilding the same view here just picks up where things left off
    // (mini-games are the one exception: their onStart() always resets round/pattern state, so a
    // burial mid-game restarts that game rather than resuming it - acceptable, not a crash).
    switch (ctx->activeView) {
        case ViewType::Menu:                showMenuView(ctx); break;
        case ViewType::Stats:                showStatsView(ctx); break;
        case ViewType::Settings:              showSettingsView(ctx); break;
        case ViewType::PatternGameView:       showPatternGame(ctx); break;
        case ViewType::ReactionGameView:      showReactionGame(ctx); break;
        case ViewType::CemeteryViewType:      showCemeteryView(ctx); break;
        case ViewType::AchievementsViewType:  showAchievementsView(ctx); break;
        case ViewType::Main:
        case ViewType::None:
        default:
            showMainView(ctx);
            break;
    }
}

void tamaTacTeardown(Context* ctx) {
    if (ctx->updateTimer != nullptr) {
        xTimerStop(ctx->updateTimer, portMAX_DELAY);
        xTimerDelete(ctx->updateTimer, portMAX_DELAY);
        ctx->updateTimer = nullptr;
    }

    // Acquire mutex to guarantee any in-flight timer callback has completed. The callback holds
    // this mutex for its entire duration, so taking it here blocks until the callback is done -
    // no arbitrary delay needed. (Belt-and-suspenders: updateTimer is already stopped/deleted
    // above, but xTimerDelete only guarantees no *new* callback starts, not that one already
    // running has finished.)
    if (ctx->timerMutex != nullptr) {
        xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);
    }

    ctx->petLogic.saveState();

    if (ctx->timerMutex != nullptr) {
        xSemaphoreGive(ctx->timerMutex);
        vSemaphoreDelete(ctx->timerMutex);
        ctx->timerMutex = nullptr;
    }

    if (ctx->sfxEngine) {
        ctx->sfxEngine->stop();
        delete ctx->sfxEngine;
        ctx->sfxEngine = nullptr;
    }

    // Don't clean ctx->wrapperWidget here: window_manager_remove() already destroyed it (and
    // everything under it) by the time this runs. stopActiveView(..., false) skips that step -
    // it still runs each view's onStop() to delete any still-alive independent lv_timer_t
    // objects, which is safe (see stopActiveView()'s comment).
    stopActiveView(ctx, false);

    ctx->wrapperWidget = nullptr;
    ctx->toolbar = nullptr;
    ctx->menuButton = nullptr;
}

//==============================================================================================
// View Management
//==============================================================================================

void showMainView(Context* ctx) {
    stopActiveView(ctx, true);

    ctx->activeView = ViewType::Main;
    mainViewCreateWidgets(ctx->wrapperWidget, ctx);
    mainViewUpdateUI(ctx);
}

void showMenuView(Context* ctx) {
    stopActiveView(ctx, true);

    ctx->activeView = ViewType::Menu;
    menuViewCreateWidgets(ctx->wrapperWidget, ctx);
}

void showStatsView(Context* ctx) {
    stopActiveView(ctx, true);

    ctx->activeView = ViewType::Stats;
    statsViewCreateWidgets(ctx->wrapperWidget, ctx);
    statsViewUpdateStats(ctx);
}

void showSettingsView(Context* ctx) {
    stopActiveView(ctx, true);

    ctx->activeView = ViewType::Settings;
    settingsViewCreateWidgets(ctx->wrapperWidget, ctx);
}

void showCemeteryView(Context* ctx) {
    stopActiveView(ctx, true);

    ctx->activeView = ViewType::CemeteryViewType;
    cemeteryViewCreateWidgets(ctx->wrapperWidget, ctx);
}

void showAchievementsView(Context* ctx) {
    stopActiveView(ctx, true);

    ctx->activeView = ViewType::AchievementsViewType;
    achievementsViewCreateWidgets(ctx->wrapperWidget, ctx);
}

void showPatternGame(Context* ctx) {
    stopActiveView(ctx, true);
    ctx->activeView = ViewType::PatternGameView;
    patternGameCreateWidgets(ctx->wrapperWidget, ctx);
}

void showReactionGame(Context* ctx) {
    stopActiveView(ctx, true);
    ctx->activeView = ViewType::ReactionGameView;
    reactionGameCreateWidgets(ctx->wrapperWidget, ctx);
}

//==============================================================================================
// Settings Handlers
//==============================================================================================

void tamaTacSetSoundEnabled(Context* ctx, bool enabled) {
    if (ctx->sfxEngine) {
        ctx->sfxEngine->setEnabled(enabled);
    }
}

void tamaTacSetDecaySpeed(Context* ctx, DecaySpeed speed) {
    ctx->decaySpeed = speed;
    PetLogic::setDecaySpeed(static_cast<uint8_t>(speed));
}

//==============================================================================================
// Action Handlers (called by MainView)
//==============================================================================================

void tamaTacHandleFeedAction(Context* ctx) {
    if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

    ctx->petLogic.performAction(PetAction::Feed);
    ctx->petLogic.saveState();
    mainViewUpdateUI(ctx);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Feed);

    achievementsUnlock(AchievementId::FirstFeed);

    char msg[64];
    snprintf(msg, sizeof(msg), "Fed! Hunger: %d%%", ctx->petLogic.getHunger());
    mainViewSetStatusText(ctx, msg);

    if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);
}

void tamaTacHandlePlayAction(Context* ctx) {
    if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

    bool canPlay = !ctx->petLogic.isDead();
    DayPhase phase = canPlay ? ctx->petLogic.getDayPhase() : DayPhase::Day;

    if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);

    if (canPlay) {
        achievementsUnlock(AchievementId::FirstPlay);
        if (phase == DayPhase::Night) {
            achievementsUnlock(AchievementId::NightOwl);
        }
        if (rand() % 2 == 0) {
            showPatternGame(ctx);
        } else {
            showReactionGame(ctx);
        }
    }
}

void tamaTacHandlePetTap(Context* ctx) {
    if (ctx->activeView != ViewType::Main) return;

    if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

    if (ctx->petLogic.isDead()) {
        if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);
        return;
    }

    // 3-second cooldown between pets
    uint32_t now = tt::kernel::getMillis();
    if (now - ctx->lastPetTime < 3000) {
        if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);
        return;
    }
    ctx->lastPetTime = now;

    ctx->petLogic.performAction(PetAction::Pet);
    ctx->petLogic.saveState();
    mainViewUpdateUI(ctx);

    if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Chirp);
}

void tamaTacOnReactionGameComplete(Context* ctx, int score, bool won) {
    if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

    int clampedScore = std::max(0, std::min(score, REACTION_GAME_MAX_ROUNDS));
    ctx->petLogic.applyPlayResult(clampedScore, REACTION_GAME_MAX_ROUNDS);
    ctx->petLogic.saveState();

    if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);

    if (won) achievementsUnlock(AchievementId::PerfectGame);
    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Play);

    showMainView(ctx);
}

void tamaTacOnPatternGameComplete(Context* ctx, int score, bool won) {
    if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

    int clampedScore = std::max(0, std::min(score, PATTERN_GAME_MAX_ROUNDS));
    ctx->petLogic.applyPlayResult(clampedScore, PATTERN_GAME_MAX_ROUNDS);
    ctx->petLogic.saveState();

    if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);

    if (won) achievementsUnlock(AchievementId::PerfectGame);
    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Play);

    showMainView(ctx);
}

void tamaTacHandleMedicineAction(Context* ctx) {
    if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

    bool wasSick = ctx->petLogic.isSick();
    ctx->petLogic.performAction(PetAction::Medicine);
    ctx->petLogic.saveState();
    mainViewUpdateUI(ctx);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Medicine);

    if (wasSick && !ctx->petLogic.isSick()) {
        achievementsUnlock(AchievementId::FirstCure);
        mainViewSetStatusText(ctx, "Cured!");
    } else {
        mainViewSetStatusText(ctx, "Medicine given");
    }

    if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);
}

void tamaTacHandleSleepAction(Context* ctx) {
    if (ctx->timerMutex) xSemaphoreTake(ctx->timerMutex, portMAX_DELAY);

    ctx->petLogic.performAction(PetAction::Sleep);
    ctx->petLogic.saveState();
    mainViewUpdateUI(ctx);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Sleep);

    char msg[64];
    snprintf(msg, sizeof(msg), "Sleeping... Energy: %d%%", ctx->petLogic.getEnergy());
    mainViewSetStatusText(ctx, msg);

    if (ctx->timerMutex) xSemaphoreGive(ctx->timerMutex);
}
