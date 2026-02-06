/**
 * @file Snake.cpp
 * @brief Snake game app implementation for Tactility
 */
#include "Snake.h"

#include <inttypes.h>
#include <tt_hal.h>
#include <tt_lvgl_toolbar.h>
#include <tt_app_alertdialog.h>
#include <tt_app_selectiondialog.h>
#include <tt_preferences.h>
#include <TactilityCpp/LvglLock.h>

constexpr auto* TAG = "Snake";

// Preferences keys for high scores (one per difficulty)
static constexpr const char* PREF_NAMESPACE = "Snake";
static constexpr const char* PREF_HIGH_EASY = "high_easy";
static constexpr const char* PREF_HIGH_MED = "high_med";
static constexpr const char* PREF_HIGH_HARD = "high_hard";
static constexpr const char* PREF_HIGH_HELL = "high_hell";

// High scores for each difficulty (loaded from preferences)
static int32_t highScoreEasy = 0;
static int32_t highScoreMedium = 0;
static int32_t highScoreHard = 0;
static int32_t highScoreHell = 0;

static constexpr size_t DIFFICULTY_COUNT = 4;

// Selection dialog indices (0 = How to Play, 1-4 = difficulties)
static constexpr int32_t SELECTION_HOW_TO_PLAY = 0;
static constexpr int32_t SELECTION_EASY = 1;
static constexpr int32_t SELECTION_MEDIUM = 2;
static constexpr int32_t SELECTION_HARD = 3;
static constexpr int32_t SELECTION_HELL = 4;

// Difficulty options (cell sizes - larger = easier)
// Hell uses same size as Hard but with wall collision enabled
static const uint16_t difficultySizes[DIFFICULTY_COUNT] = { SNAKE_CELL_LARGE, SNAKE_CELL_MEDIUM, SNAKE_CELL_SMALL, SNAKE_CELL_SMALL };

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
        tt_preferences_opt_int32(prefs, PREF_HIGH_EASY, &highScoreEasy);
        tt_preferences_opt_int32(prefs, PREF_HIGH_MED, &highScoreMedium);
        tt_preferences_opt_int32(prefs, PREF_HIGH_HARD, &highScoreHard);
        tt_preferences_opt_int32(prefs, PREF_HIGH_HELL, &highScoreHell);
        tt_preferences_free(prefs);
    }
}

static void saveHighScore(int32_t difficulty, int32_t score) {
    PreferencesHandle prefs = tt_preferences_alloc(PREF_NAMESPACE);
    if (prefs) {
        switch (difficulty) {
            case SELECTION_EASY:
                highScoreEasy = score;
                tt_preferences_put_int32(prefs, PREF_HIGH_EASY, score);
                break;
            case SELECTION_MEDIUM:
                highScoreMedium = score;
                tt_preferences_put_int32(prefs, PREF_HIGH_MED, score);
                break;
            case SELECTION_HARD:
                highScoreHard = score;
                tt_preferences_put_int32(prefs, PREF_HIGH_HARD, score);
                break;
            case SELECTION_HELL:
                highScoreHell = score;
                tt_preferences_put_int32(prefs, PREF_HIGH_HELL, score);
                break;
        }
        tt_preferences_free(prefs);
    }
}

static int32_t getHighScore(int32_t difficulty) {
    switch (difficulty) {
        case SELECTION_EASY: return highScoreEasy;
        case SELECTION_MEDIUM: return highScoreMedium;
        case SELECTION_HARD: return highScoreHard;
        case SELECTION_HELL: return highScoreHell;
        default: return 0;
    }
}

void Snake::showHelpDialog() {
    const char* buttons[] = { "OK" };
    helpDialogId = tt_app_alertdialog_start(
        "How to Play",
        "Swipe or use arrow keys to change direction.\n"
        "Eat food to grow longer.\n"
        "Don't hit yourself!",
        buttons, 1);
}

void Snake::showSelectionDialog() {
    const char* items[] = { "How to Play", "Easy", "Medium", "Hard", "Hell" };
    selectionDialogId = tt_app_selectiondialog_start("Snake", 5, items);
}

void Snake::snakeEventCb(lv_event_t* e) {
    Snake* self = (Snake*)lv_event_get_user_data(e);
    lv_obj_t* target = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        if (snake_get_game_over(target)) {
            int32_t score = snake_get_score(target);
            int32_t length = snake_get_length(target);
            int32_t prevHighScore = getHighScore(self->currentDifficulty);
            bool isNewHighScore = score > prevHighScore;

            // Save high score if it's a new record
            if (isNewHighScore) {
                saveHighScore(self->currentDifficulty, score);
            }

            const char* alertDialogLabels[] = { "OK" };
            char message[120];
            if (isNewHighScore && score > 0) {
                snprintf(message, sizeof(message), "NEW HIGH SCORE!\n\nSCORE: %" PRId32 "\nLENGTH: %" PRId32,
                        score, length);
            } else {
                snprintf(message, sizeof(message), "GAME OVER!\n\nSCORE: %" PRId32 "\nLENGTH: %" PRId32 "\nBEST: %" PRId32,
                        score, length, getHighScore(self->currentDifficulty));
            }
            self->gameOverDialogId = tt_app_alertdialog_start(
                isNewHighScore && score > 0 ? "NEW HIGH SCORE!" : "GAME OVER!",
                message, alertDialogLabels, 1);
        } else {
            // Update score display
            lv_label_set_text_fmt(self->scoreLabel, "SCORE: %u", snake_get_score(self->gameObject));
        }
    }
}

void Snake::newGameBtnEvent(lv_event_t* e) {
    Snake* self = (Snake*)lv_event_get_user_data(e);
    if (self == nullptr) {
        return;
    }
    snake_set_new_game(self->gameObject);
    // Update score label
    if (self->scoreLabel) {
        lv_label_set_text_fmt(self->scoreLabel, "SCORE: %u", snake_get_score(self->gameObject));
    }
}

void Snake::createGame(lv_obj_t* parent, uint16_t cell_size, bool wallCollision, lv_obj_t* tb) {
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    // Create game widget
    gameObject = snake_create(parent, cell_size, wallCollision);
    if (!gameObject) {
        return;
    }
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
    lv_label_set_text_fmt(scoreLabel, "SCORE: %u", snake_get_score(gameObject));
    lv_obj_set_style_text_align(scoreLabel, LV_TEXT_ALIGN_LEFT, LV_STATE_DEFAULT);
    lv_obj_align(scoreLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(scoreLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(scoreLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(scoreLabel, lv_palette_main(LV_PALETTE_GREEN), LV_PART_MAIN);
    lv_obj_add_event_cb(gameObject, snakeEventCb, LV_EVENT_VALUE_CHANGED, this);

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

void Snake::onHide(AppHandle appHandle) {
    scoreLabel = nullptr;
    scoreWrapper = nullptr;
    toolbar = nullptr;
    mainWrapper = nullptr;
    newGameWrapper = nullptr;
    gameObject = nullptr;
}

void Snake::onShow(AppHandle appHandle, lv_obj_t* parent) {
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
    // Check if we have a pending difficulty selection from onResult
    } else if (pendingSelection >= SELECTION_EASY && pendingSelection <= SELECTION_HELL) {
        // Force layout update before creating game so dimensions are computed
        lv_obj_update_layout(parent);
        // Track which difficulty we're playing for high score saving
        currentDifficulty = pendingSelection;
        // Start game with selected difficulty (convert selection index to difficulty index)
        int32_t difficultyIndex = pendingSelection - SELECTION_EASY;
        // Hell mode enables wall collision (hitting walls = game over)
        bool wallCollision = (pendingSelection == SELECTION_HELL);
        createGame(mainWrapper, difficultySizes[difficultyIndex], wallCollision, toolbar);
        pendingSelection = -1;
    } else {
        // Show selection dialog
        showSelectionDialog();
    }
}

void Snake::onResult(AppHandle appHandle, void* _Nullable data, AppLaunchId launchId, AppResult result, BundleHandle resultData) {
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
        } else if (selection >= SELECTION_EASY && selection <= SELECTION_HELL) {
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
    }
}
