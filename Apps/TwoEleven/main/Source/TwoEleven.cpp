/**
 * @file TwoEleven.cpp
 * @brief 2048 game app implementation for Tactility
 */
#include "TwoEleven.h"

#include <inttypes.h>
#include <tt_hal.h>
#include <tt_lvgl_toolbar.h>
#include <tt_app_alertdialog.h>
#include <tt_app_selectiondialog.h>
#include <tt_preferences.h>
#include <TactilityCpp/LvglLock.h>

constexpr auto* TAG = "TwoEleven";

// Preferences keys for high scores (one per grid size)
static constexpr const char* PREF_NAMESPACE = "TwoEleven";
static constexpr const char* PREF_HIGH_3X3 = "high_3x3";
static constexpr const char* PREF_HIGH_4X4 = "high_4x4";
static constexpr const char* PREF_HIGH_5X5 = "high_5x5";
static constexpr const char* PREF_HIGH_6X6 = "high_6x6";

// High scores for each grid size (loaded from preferences)
static int32_t highScore3x3 = 0;
static int32_t highScore4x4 = 0;
static int32_t highScore5x5 = 0;
static int32_t highScore6x6 = 0;

static constexpr size_t SIZE_COUNT = 4;

// Selection dialog indices (0 = How to Play, 1-4 = grid sizes)
static constexpr int32_t SELECTION_HOW_TO_PLAY = 0;
static constexpr int32_t SELECTION_3X3 = 1;
static constexpr int32_t SELECTION_4X4 = 2;
static constexpr int32_t SELECTION_5X5 = 3;
static constexpr int32_t SELECTION_6X6 = 4;

// Grid size options (index matches selection - 1)
static const uint16_t gridSizes[SIZE_COUNT] = { 3, 4, 5, 6 };

static int getToolbarHeight(UiScale uiScale) {
    if (uiScale == UiScale::UiScaleSmallest) {
        return 22;
    } else {
        return 40;
    }
}

static void loadHighScores() {
    PreferencesHandle prefs = tt_preferences_alloc(PREF_NAMESPACE);
    if (prefs) {
        tt_preferences_opt_int32(prefs, PREF_HIGH_3X3, &highScore3x3);
        tt_preferences_opt_int32(prefs, PREF_HIGH_4X4, &highScore4x4);
        tt_preferences_opt_int32(prefs, PREF_HIGH_5X5, &highScore5x5);
        tt_preferences_opt_int32(prefs, PREF_HIGH_6X6, &highScore6x6);
        tt_preferences_free(prefs);
    }
}

static void saveHighScore(int32_t gridSize, int32_t score) {
    PreferencesHandle prefs = tt_preferences_alloc(PREF_NAMESPACE);
    if (prefs) {
        switch (gridSize) {
            case SELECTION_3X3:
                highScore3x3 = score;
                tt_preferences_put_int32(prefs, PREF_HIGH_3X3, score);
                break;
            case SELECTION_4X4:
                highScore4x4 = score;
                tt_preferences_put_int32(prefs, PREF_HIGH_4X4, score);
                break;
            case SELECTION_5X5:
                highScore5x5 = score;
                tt_preferences_put_int32(prefs, PREF_HIGH_5X5, score);
                break;
            case SELECTION_6X6:
                highScore6x6 = score;
                tt_preferences_put_int32(prefs, PREF_HIGH_6X6, score);
                break;
        }
        tt_preferences_free(prefs);
    }
}

static int32_t getHighScore(int32_t gridSize) {
    switch (gridSize) {
        case SELECTION_3X3: return highScore3x3;
        case SELECTION_4X4: return highScore4x4;
        case SELECTION_5X5: return highScore5x5;
        case SELECTION_6X6: return highScore6x6;
        default: return 0;
    }
}

void TwoEleven::showSelectionDialog() {
    const char* items[] = { "How to Play", "3x3", "4x4", "5x5", "6x6" };
    selectionDialogId = tt_app_selectiondialog_start("2048", 5, items);
}

void TwoEleven::showHelpDialog() {
    const char* buttons[] = { "OK" };
    helpDialogId = tt_app_alertdialog_start(
        "How to Play",
        "Swipe or use arrow keys to move tiles.\n"
        "Tiles with the same number merge.\n"
        "Reach 2048 to win!",
        buttons, 1);
}

void TwoEleven::twoElevenEventCb(lv_event_t* e) {
    TwoEleven* self = (TwoEleven*)lv_event_get_user_data(e);
    if (self == nullptr) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        int32_t score = twoeleven_get_score(self->gameObject);

        if (self->gameOverDialogId == 0 && twoeleven_get_best_tile(self->gameObject) >= 2048) {
            int32_t prevHighScore = getHighScore(self->currentGridSize);
            bool isNewHighScore = score > prevHighScore;

            // Save high score if it's a new record
            if (isNewHighScore) {
                saveHighScore(self->currentGridSize, score);
            }

            const char* alertDialogLabels[] = { "OK" };
            char message[100];
            if (isNewHighScore) {
                snprintf(message, sizeof(message), "NEW HIGH SCORE!\n\nSCORE: %" PRId32, score);
                self->gameOverDialogId = tt_app_alertdialog_start("YOU WIN!", message, alertDialogLabels, 1);
            } else {
                snprintf(message, sizeof(message), "YOU WIN!\n\nSCORE: %" PRId32 "\nBEST: %" PRId32, score, getHighScore(self->currentGridSize));
                self->gameOverDialogId = tt_app_alertdialog_start("YOU WIN!", message, alertDialogLabels, 1);
            }
        } else if (self->gameOverDialogId == 0 && twoeleven_get_status(self->gameObject)) {
            int32_t prevHighScore = getHighScore(self->currentGridSize);
            bool isNewHighScore = score > prevHighScore;

            // Save high score if it's a new record
            if (isNewHighScore) {
                saveHighScore(self->currentGridSize, score);
            }

            const char* alertDialogLabels[] = { "OK" };
            char message[100];
            if (isNewHighScore && score > 0) {
                snprintf(message, sizeof(message), "NEW HIGH SCORE!\n\nSCORE: %" PRId32, score);
                self->gameOverDialogId = tt_app_alertdialog_start("NEW HIGH SCORE!", message, alertDialogLabels, 1);
            } else {
                snprintf(message, sizeof(message), "GAME OVER!\n\nSCORE: %" PRId32 "\nBEST: %" PRId32, score, getHighScore(self->currentGridSize));
                self->gameOverDialogId = tt_app_alertdialog_start("GAME OVER!", message, alertDialogLabels, 1);
            }
        } else {
            // Update score display
            lv_label_set_text_fmt(self->scoreLabel, "SCORE: %" PRId32, score);
        }
    }
}

void TwoEleven::newGameBtnEvent(lv_event_t* e) {
    TwoEleven* self = (TwoEleven*)lv_event_get_user_data(e);
    if (self == nullptr) {
        return;
    }
    twoeleven_set_new_game(self->gameObject);
    // Update score label
    if (self->scoreLabel) {
        lv_label_set_text_fmt(self->scoreLabel, "SCORE: %" PRId32, twoeleven_get_score(self->gameObject));
    }
}

void TwoEleven::createGame(lv_obj_t* parent, uint16_t size, lv_obj_t* tb) {
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    // Create game widget
    gameObject = twoeleven_create(parent, size);
    lv_obj_set_style_text_font(gameObject, lv_font_get_default(), 0);
    lv_obj_set_size(gameObject, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(gameObject, 1);

    // Create score wrapper in toolbar
    scoreWrapper = lv_obj_create(tb);
    lv_obj_set_size(scoreWrapper, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_pad_top(scoreWrapper, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(scoreWrapper, 10, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(scoreWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Create score label
    scoreLabel = lv_label_create(scoreWrapper);
    lv_label_set_text_fmt(scoreLabel, "SCORE: %" PRId32, twoeleven_get_score(gameObject));
    lv_obj_set_style_text_align(scoreLabel, LV_TEXT_ALIGN_LEFT, LV_STATE_DEFAULT);
    lv_obj_align(scoreLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(scoreLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(scoreLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(scoreLabel, lv_palette_main(LV_PALETTE_AMBER), LV_PART_MAIN);
    lv_obj_add_event_cb(gameObject, twoElevenEventCb, LV_EVENT_VALUE_CHANGED, this);

    // Create new game button wrapper
    newGameWrapper = lv_obj_create(tb);
    lv_obj_set_width(newGameWrapper, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(newGameWrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(newGameWrapper, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(newGameWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(newGameWrapper, 0, LV_STATE_DEFAULT);

    // Create new game button
    auto ui_scale = tt_hal_configuration_get_ui_scale();
    auto toolbar_height = getToolbarHeight(ui_scale);
    lv_obj_t* newGameBtn = lv_btn_create(newGameWrapper);
    if (ui_scale == UiScale::UiScaleSmallest) {
        lv_obj_set_size(newGameBtn, toolbar_height - 8, toolbar_height - 8);
    } else {
        lv_obj_set_size(newGameBtn, toolbar_height - 6, toolbar_height - 6);
    }
    lv_obj_set_style_pad_all(newGameBtn, 0, LV_STATE_DEFAULT);
    lv_obj_align(newGameBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(newGameBtn, newGameBtnEvent, LV_EVENT_CLICKED, this);

    lv_obj_t* btnIcon = lv_image_create(newGameBtn);
    lv_image_set_src(btnIcon, LV_SYMBOL_REFRESH);
    lv_obj_align(btnIcon, LV_ALIGN_CENTER, 0, 0);
}

void TwoEleven::onHide(AppHandle appHandle) {
    scoreLabel = nullptr;
    scoreWrapper = nullptr;
    toolbar = nullptr;
    mainWrapper = nullptr;
    newGameWrapper = nullptr;
    gameObject = nullptr;
}

void TwoEleven::onShow(AppHandle appHandle, lv_obj_t* parent) {
    // Check if we should exit (user closed selection dialog)
    if (shouldExit) {
        shouldExit = false;
        tt_app_stop();
        return;
    }

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    // Create toolbar
    toolbar = tt_lvgl_toolbar_create_for_app(parent, appHandle);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    // Create main wrapper
    mainWrapper = lv_obj_create(parent);
    lv_obj_set_width(mainWrapper, LV_PCT(100));
    lv_obj_set_flex_grow(mainWrapper, 1);
    lv_obj_set_style_pad_all(mainWrapper, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(mainWrapper, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(mainWrapper, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(mainWrapper, 0, LV_PART_MAIN);
    lv_obj_remove_flag(mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Load high scores on first show
    if (!highScoresLoaded) {
        loadHighScores();
        highScoresLoaded = true;
    }

    // Check if we need to show the help dialog
    if (showHelpOnShow) {
        showHelpOnShow = false;
        showHelpDialog();
    // Check if we have a pending size selection from onResult
    } else if (pendingSelection >= SELECTION_3X3 && pendingSelection <= SELECTION_6X6) {
        // Force layout update before creating game so dimensions are computed
        lv_obj_update_layout(parent);
        // Track which grid size we're playing for high score saving
        currentGridSize = pendingSelection;
        // Start game with selected size (convert selection index to size index)
        int32_t sizeIndex = pendingSelection - SELECTION_3X3;
        createGame(mainWrapper, gridSizes[sizeIndex], toolbar);
        pendingSelection = -1;
    } else {
        // Show selection dialog
        showSelectionDialog();
    }
}

void TwoEleven::onResult(AppHandle appHandle, void* _Nullable data, AppLaunchId launchId, AppResult result, BundleHandle resultData) {
    // Don't manipulate LVGL objects here - they may be invalid
    // Just store state for onShow to handle

    if (launchId == selectionDialogId && selectionDialogId != 0) {
        selectionDialogId = 0;

        int32_t selection = -1;
        if (resultData != nullptr) {
            selection = tt_app_selectiondialog_get_result_index(resultData);
        }

        if (selection == SELECTION_HOW_TO_PLAY) {
            // Mark to show help dialog in onShow
            showHelpOnShow = true;
        } else if (selection >= SELECTION_3X3 && selection <= SELECTION_6X6) {
            // Store selection for onShow to handle
            pendingSelection = selection;
        } else {
            // User closed dialog without selecting - mark for exit
            shouldExit = true;
        }

    } else if (launchId == helpDialogId && helpDialogId != 0) {
        helpDialogId = 0;
        // Return to selection dialog
        pendingSelection = -1;

    } else if (launchId == gameOverDialogId && gameOverDialogId != 0) {
        gameOverDialogId = 0;
        // Mark to show selection dialog in onShow
        pendingSelection = -1;

    } else if (launchId == winDialogId && winDialogId != 0) {
        winDialogId = 0;
        // Mark to show selection dialog in onShow
        pendingSelection = -1;
    }
}
