/**
 * @file TamaTac.cpp
 * @brief TamaTac virtual pet app implementation
 */

#include "TamaTac.h"
#include "SpriteData.h"
#include <tt_lvgl_toolbar.h>
#include <tt_app_alertdialog.h>
#include <Tactility/kernel/Kernel.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

// Static member initialization
PetLogic* TamaTac::petLogic = nullptr;
TimerHandle_t TamaTac::updateTimer = nullptr;
SemaphoreHandle_t TamaTac::timerMutex = nullptr;
AppHandle TamaTac::currentApp = nullptr;
AppLaunchId TamaTac::resetDialogId = 0;
LifeStage TamaTac::lastKnownStage = LifeStage::Egg;
SfxEngine* TamaTac::sfxEngine = nullptr;
DecaySpeed TamaTac::decaySpeed = DecaySpeed::Normal;
uint32_t TamaTac::lastPetTime = 0;
bool TamaTac::pendingResetUI = false;

// Canvas buffer definitions (shared by views via extern)
// 72x72 supports 24x24 sprites at 3x scale (medium/large/xlarge screens)
lv_color_t TamaTac_canvasBuffer[72 * 72];
lv_color_t TamaTac_iconBuffers[12][16 * 16];

void TamaTac::onCreate(AppHandle app) {
    currentApp = app;

    // Seed RNG once so rand() produces different sequences each run
    srand(tt::kernel::getMillis());

    if (petLogic == nullptr) {
        petLogic = new PetLogic();

        if (!petLogic->loadState()) {
            // No save data - pet starts fresh
        }

        lastKnownStage = petLogic->getStats().stage;
    }

    if (timerMutex == nullptr) {
        timerMutex = xSemaphoreCreateMutex();
    }
}

void TamaTac::onShow(AppHandle context, lv_obj_t* parent) {
    // Initialize SfxEngine
    if (sfxEngine == nullptr) {
        sfxEngine = new SfxEngine();
        sfxEngine->start();
        sfxEngine->applyVolumePreset(SfxEngine::VolumePreset::Normal);

        // Load settings
        bool soundEnabled;
        SettingsView::loadSettings(&soundEnabled, &decaySpeed);
        sfxEngine->setEnabled(soundEnabled);
        PetLogic::setDecaySpeed(static_cast<uint8_t>(decaySpeed));
    }

    if (updateTimer == nullptr) {
        updateTimer = xTimerCreate(
            "PetUpdate",
            pdMS_TO_TICKS(30000),
            pdTRUE,
            nullptr,
            onTimerUpdate
        );

        if (updateTimer != nullptr) {
            xTimerStart(updateTimer, 0);
        }
    }

    currentApp = context;

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    toolbar = tt_lvgl_toolbar_create_for_app(parent, context);

    menuButton = tt_lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_LIST, onMenuClicked, this);
    tt_lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_TRASH, onCleanClicked, this);
    tt_lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_REFRESH, onResetClicked, this);

    wrapperWidget = lv_obj_create(parent);
    lv_obj_set_width(wrapperWidget, LV_PCT(100));
    lv_obj_set_flex_grow(wrapperWidget, 1);
    lv_obj_set_style_pad_all(wrapperWidget, 0, 0);
    lv_obj_set_style_border_width(wrapperWidget, 0, 0);
    lv_obj_set_style_bg_opa(wrapperWidget, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(wrapperWidget, LV_OBJ_FLAG_SCROLLABLE);

    showMainView();
}

void TamaTac::onHide(AppHandle context) {
    if (updateTimer != nullptr) {
        xTimerStop(updateTimer, portMAX_DELAY);
        xTimerDelete(updateTimer, portMAX_DELAY);
        updateTimer = nullptr;
    }

    if (sfxEngine) {
        sfxEngine->stop();
        delete sfxEngine;
        sfxEngine = nullptr;
    }

    stopActiveView();

    wrapperWidget = nullptr;
    toolbar = nullptr;
    menuButton = nullptr;
}

void TamaTac::onDestroy(AppHandle app) {
    // Acquire mutex to guarantee any in-flight timer callback has completed.
    // The callback holds this mutex for its entire duration, so taking it
    // here blocks until the callback is done — no arbitrary delay needed.
    if (timerMutex != nullptr) {
        xSemaphoreTake(timerMutex, portMAX_DELAY);
    }

    if (petLogic) {
        petLogic->saveState();
        delete petLogic;
        petLogic = nullptr;
    }

    if (timerMutex != nullptr) {
        xSemaphoreGive(timerMutex);
        vSemaphoreDelete(timerMutex);
        timerMutex = nullptr;
    }

    currentApp = nullptr;
}

void TamaTac::onResult(AppHandle app, void* data, AppLaunchId launchId, AppResult result, BundleHandle resultData) {
    if (launchId == resetDialogId) {
        int32_t buttonIndex = tt_app_alertdialog_get_result_index(resultData);
        if (buttonIndex == 0) {
            // User clicked "Reset" (first button)
            if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

            if (petLogic) {
                petLogic->reset();
                petLogic->saveState();
                lastKnownStage = LifeStage::Egg;
                pendingResetUI = true;  // Defer UI update to LVGL task
            }

            if (timerMutex) xSemaphoreGive(timerMutex);
        }
        resetDialogId = 0;
    }
}

//==============================================================================================
// View Management
//==============================================================================================

void TamaTac::stopActiveView() {
    switch (activeView) {
        case ViewType::Main:
            mainView.onStop();
            break;
        case ViewType::Menu:
            menuView.onStop();
            break;
        case ViewType::Stats:
            statsView.onStop();
            break;
        case ViewType::Settings:
            settingsView.onStop();
            break;
        case ViewType::PatternGameView:
            patternGame.onStop();
            break;
        case ViewType::ReactionGameView:
            reactionGame.onStop();
            break;
        case ViewType::CemeteryViewType:
            cemeteryView.onStop();
            break;
        case ViewType::AchievementsViewType:
            achievementsView.onStop();
            break;
        case ViewType::None:
            break;
    }

    if (wrapperWidget) {
        lv_obj_clean(wrapperWidget);
    }

    activeView = ViewType::None;
}

void TamaTac::showMainView() {
    stopActiveView();

    activeView = ViewType::Main;
    mainView.onStart(wrapperWidget, this);
    mainView.updateUI(petLogic, lastKnownStage);
}

void TamaTac::showMenuView() {
    stopActiveView();

    activeView = ViewType::Menu;
    menuView.onStart(wrapperWidget, this);
}

void TamaTac::showStatsView() {
    stopActiveView();

    activeView = ViewType::Stats;
    statsView.onStart(wrapperWidget, this);
    statsView.updateStats(petLogic);
}

void TamaTac::showSettingsView() {
    stopActiveView();

    activeView = ViewType::Settings;
    settingsView.onStart(wrapperWidget, this);
}

void TamaTac::showCemeteryView() {
    stopActiveView();

    activeView = ViewType::CemeteryViewType;
    cemeteryView.onStart(wrapperWidget, this);
}

void TamaTac::showAchievementsView() {
    stopActiveView();

    activeView = ViewType::AchievementsViewType;
    achievementsView.onStart(wrapperWidget, this);
}

//==============================================================================================
// Settings Handlers
//==============================================================================================

void TamaTac::setSoundEnabled(bool enabled) {
    if (sfxEngine) {
        sfxEngine->setEnabled(enabled);
    }
}

void TamaTac::setDecaySpeed(DecaySpeed speed) {
    decaySpeed = speed;
    PetLogic::setDecaySpeed(static_cast<uint8_t>(speed));
}

//==============================================================================================
// Action Handlers (called by MainView)
//==============================================================================================

void TamaTac::handleFeedAction() {
    if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

    if (petLogic) {
        petLogic->performAction(PetAction::Feed);
        petLogic->saveState();
        mainView.updateUI(petLogic, lastKnownStage);

        if (sfxEngine) sfxEngine->play(SfxId::Feed);

        AchievementsView::unlock(AchievementId::FirstFeed);

        char msg[64];
        snprintf(msg, sizeof(msg), "Fed! Hunger: %d%%", petLogic->getHunger());
        mainView.setStatusText(msg);
    }

    if (timerMutex) xSemaphoreGive(timerMutex);
}

void TamaTac::handlePlayAction() {
    if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

    bool canPlay = petLogic && !petLogic->isDead();
    DayPhase phase = canPlay ? petLogic->getDayPhase() : DayPhase::Day;

    if (timerMutex) xSemaphoreGive(timerMutex);

    if (canPlay) {
        AchievementsView::unlock(AchievementId::FirstPlay);
        if (phase == DayPhase::Night) {
            AchievementsView::unlock(AchievementId::NightOwl);
        }
        if (rand() % 2 == 0) {
            showPatternGame();
        } else {
            showReactionGame();
        }
    }
}

void TamaTac::handlePetTap() {
    if (activeView != ViewType::Main) return;

    if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

    if (!petLogic || petLogic->isDead()) {
        if (timerMutex) xSemaphoreGive(timerMutex);
        return;
    }

    // 3-second cooldown between pets
    uint32_t now = tt::kernel::getMillis();
    if (now - lastPetTime < 3000) {
        if (timerMutex) xSemaphoreGive(timerMutex);
        return;
    }
    lastPetTime = now;

    petLogic->performAction(PetAction::Pet);
    petLogic->saveState();
    mainView.updateUI(petLogic, lastKnownStage);

    if (timerMutex) xSemaphoreGive(timerMutex);

    if (sfxEngine) sfxEngine->play(SfxId::Chirp);
}

void TamaTac::showPatternGame() {
    stopActiveView();
    activeView = ViewType::PatternGameView;
    patternGame.onStart(wrapperWidget, this);
}

void TamaTac::showReactionGame() {
    stopActiveView();
    activeView = ViewType::ReactionGameView;
    reactionGame.onStart(wrapperWidget, this);
}

void TamaTac::onReactionGameComplete(int score, bool won) {
    if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

    if (petLogic) {
        int clampedScore = std::max(0, std::min(score, ReactionGame::MAX_ROUNDS));
        petLogic->applyPlayResult(clampedScore, ReactionGame::MAX_ROUNDS);
        petLogic->saveState();
    }

    if (timerMutex) xSemaphoreGive(timerMutex);

    if (won) AchievementsView::unlock(AchievementId::PerfectGame);
    if (sfxEngine) sfxEngine->play(SfxId::Play);

    showMainView();
}

void TamaTac::onPatternGameComplete(int score, bool won) {
    if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

    if (petLogic) {
        int clampedScore = std::max(0, std::min(score, PatternGame::MAX_ROUNDS));
        petLogic->applyPlayResult(clampedScore, PatternGame::MAX_ROUNDS);
        petLogic->saveState();
    }

    if (timerMutex) xSemaphoreGive(timerMutex);

    if (won) AchievementsView::unlock(AchievementId::PerfectGame);
    if (sfxEngine) sfxEngine->play(SfxId::Play);

    showMainView();
}

void TamaTac::handleMedicineAction() {
    if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

    if (petLogic) {
        bool wasSick = petLogic->isSick();
        petLogic->performAction(PetAction::Medicine);
        petLogic->saveState();
        mainView.updateUI(petLogic, lastKnownStage);

        if (sfxEngine) sfxEngine->play(SfxId::Medicine);

        if (wasSick && !petLogic->isSick()) {
            AchievementsView::unlock(AchievementId::FirstCure);
            mainView.setStatusText("Cured!");
        } else {
            mainView.setStatusText("Medicine given");
        }
    }

    if (timerMutex) xSemaphoreGive(timerMutex);
}

void TamaTac::handleSleepAction() {
    if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

    if (petLogic) {
        petLogic->performAction(PetAction::Sleep);
        petLogic->saveState();
        mainView.updateUI(petLogic, lastKnownStage);

        if (sfxEngine) sfxEngine->play(SfxId::Sleep);

        char msg[64];
        snprintf(msg, sizeof(msg), "Sleeping... Energy: %d%%", petLogic->getEnergy());
        mainView.setStatusText(msg);
    }

    if (timerMutex) xSemaphoreGive(timerMutex);
}

//==============================================================================================
// Timer Callback
//==============================================================================================

void TamaTac::onTimerUpdate(TimerHandle_t timer) {
    // Hold mutex for entire callback so onDestroy() can block until we finish
    if (timerMutex == nullptr || xSemaphoreTake(timerMutex, 0) != pdTRUE) return;

    if (petLogic == nullptr) {
        xSemaphoreGive(timerMutex);
        return;
    }

    bool wasAlive = !petLogic->isDead();
    // Capture stage before update() — checkHealth() sets stage to Ghost on death
    LifeStage stageBeforeDeath = petLogic->getStats().stage;

    uint32_t now = tt::kernel::getMillis();
    petLogic->update(now);
    petLogic->saveState();

    // Check achievements
    const PetStats& stats = petLogic->getStats();

    // Evolution achievements
    switch (stats.stage) {
        case LifeStage::Baby:  AchievementsView::unlock(AchievementId::ReachBaby); break;
        case LifeStage::Teen:  AchievementsView::unlock(AchievementId::ReachTeen); break;
        case LifeStage::Adult: AchievementsView::unlock(AchievementId::ReachAdult); break;
        case LifeStage::Elder: AchievementsView::unlock(AchievementId::ReachElder); break;
        default: break;
    }

    // Survival achievement
    if (stats.ageHours >= 24 && !stats.isDead) {
        AchievementsView::unlock(AchievementId::Survivor24h);
    }

    // Full stats achievement
    if (stats.hunger >= 90 && stats.happiness >= 90 && stats.health >= 90 && stats.energy >= 90) {
        AchievementsView::unlock(AchievementId::FullStats);
    }

    // Record death in cemetery (use pre-death stage, not Ghost)
    if (wasAlive && petLogic->isDead()) {
        CemeteryView::recordDeath(stats.personality, stageBeforeDeath, stats.ageHours);
    }

    xSemaphoreGive(timerMutex);
}

//==============================================================================================
// Event Handlers
//==============================================================================================

void TamaTac::onCleanClicked(lv_event_t* e) {
    TamaTac* app = static_cast<TamaTac*>(lv_event_get_user_data(e));
    if (app == nullptr) return;

    if (timerMutex) xSemaphoreTake(timerMutex, portMAX_DELAY);

    if (petLogic && app->activeView == ViewType::Main) {
        int poopCount = petLogic->getStats().poopCount;
        if (poopCount > 0) {
            petLogic->performAction(PetAction::Clean);
            petLogic->saveState();
            app->mainView.updateUI(petLogic, lastKnownStage);

            if (sfxEngine) sfxEngine->play(SfxId::Clean);
            AchievementsView::incrementCleanCount();

            app->mainView.setStatusText("All clean!");
        } else {
            app->mainView.setStatusText("Nothing to clean!");
        }
    }

    if (timerMutex) xSemaphoreGive(timerMutex);
}

void TamaTac::onMenuClicked(lv_event_t* e) {
    TamaTac* app = static_cast<TamaTac*>(lv_event_get_user_data(e));
    if (app == nullptr) return;

    if (app->activeView == ViewType::Main) {
        app->showMenuView();
    } else {
        app->showMainView();
    }
}

void TamaTac::onResetClicked([[maybe_unused]] lv_event_t* e) {
    const char* buttons[] = {"Reset", "Cancel"};
    resetDialogId = tt_app_alertdialog_start(
        "Reset Pet?",
        "This will start over with a new pet. Your current pet will be lost forever!",
        buttons,
        2
    );
}
