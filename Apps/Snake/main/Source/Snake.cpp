/**
 * @file Snake.cpp
 * @brief Snake game app implementation for Tactility
 */
#include "Snake.h"

#include <inttypes.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <lvgl/widgets/toolbar.h>

#include <app/manager.h>
#include <app/paths.h>
#include <tactility/preferences.h>

#include <string>

constexpr auto* TAG = "Snake";

namespace {

// Preferences keys for high scores (one per difficulty)
constexpr const char* PREF_HIGH_EASY = "high_easy";
constexpr const char* PREF_HIGH_MED = "high_med";
constexpr const char* PREF_HIGH_HARD = "high_hard";
constexpr const char* PREF_HIGH_HELL = "high_hell";

constexpr size_t DIFFICULTY_COUNT = 4;

// Difficulty options (cell sizes - larger = easier)
// Hell uses same size as Hard but with wall collision enabled
const uint16_t difficultySizes[DIFFICULTY_COUNT] = { SNAKE_CELL_LARGE, SNAKE_CELL_MEDIUM, SNAKE_CELL_SMALL, SNAKE_CELL_SMALL };

uint32_t getToolbarHeight(UiDensity uiDensity) {
    if (uiDensity == LVGL_UI_DENSITY_COMPACT) {
        return lvgl_get_text_font_height(FONT_SIZE_DEFAULT) * 1.4f;
    } else {
        return lvgl_get_text_font_height(FONT_SIZE_LARGE) * 2.2f;
    }
}

uint32_t getActionIconPadding(UiDensity uiDensity) {
    auto toolbar_height = getToolbarHeight(uiDensity);
    return (uiDensity != LVGL_UI_DENSITY_COMPACT) ? (uint32_t)(toolbar_height * 0.2f) : 8;
}

bool getPreferencesPath(std::string& outPath) {
    char path[128];
    if (app_paths_get_user_data_path("tactility.snake", "snake.properties", path, sizeof(path)) != ERROR_NONE) {
        return false;
    }
    outPath = std::string(path);
    return true;
}

void loadHighScores(Context* ctx) {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;
    preferences_opt_int32(prefs, PREF_HIGH_EASY, &ctx->highScoreEasy);
    preferences_opt_int32(prefs, PREF_HIGH_MED, &ctx->highScoreMedium);
    preferences_opt_int32(prefs, PREF_HIGH_HARD, &ctx->highScoreHard);
    preferences_opt_int32(prefs, PREF_HIGH_HELL, &ctx->highScoreHell);
    preferences_close(prefs);
}

void saveHighScore(Context* ctx, int32_t difficulty, int32_t score) {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;
    switch (difficulty) {
        case SNAKE_SELECTION_EASY:
            ctx->highScoreEasy = score;
            preferences_put_int32(prefs, PREF_HIGH_EASY, score);
            break;
        case SNAKE_SELECTION_MEDIUM:
            ctx->highScoreMedium = score;
            preferences_put_int32(prefs, PREF_HIGH_MED, score);
            break;
        case SNAKE_SELECTION_HARD:
            ctx->highScoreHard = score;
            preferences_put_int32(prefs, PREF_HIGH_HARD, score);
            break;
        case SNAKE_SELECTION_HELL:
            ctx->highScoreHell = score;
            preferences_put_int32(prefs, PREF_HIGH_HELL, score);
            break;
    }
    preferences_close(prefs);
}

int32_t getHighScore(Context* ctx, int32_t difficulty) {
    switch (difficulty) {
        case SNAKE_SELECTION_EASY: return ctx->highScoreEasy;
        case SNAKE_SELECTION_MEDIUM: return ctx->highScoreMedium;
        case SNAKE_SELECTION_HARD: return ctx->highScoreHard;
        case SNAKE_SELECTION_HELL: return ctx->highScoreHell;
        default: return 0;
    }
}

void snakeEventCb(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    lv_obj_t* target = lv_event_get_target_obj(e);
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        if (snake_get_game_over(target)) {
            int32_t score = snake_get_score(target);
            int32_t length = snake_get_length(target);
            int32_t prevHighScore = getHighScore(ctx, ctx->currentDifficulty);
            bool isNewHighScore = score > prevHighScore;

            // Save high score if it's a new record
            if (isNewHighScore) {
                saveHighScore(ctx, ctx->currentDifficulty, score);
            }

            const char* alertTitle = isNewHighScore && score > 0 ? "NEW HIGH SCORE!" : "GAME OVER!";
            char message[120];
            if (isNewHighScore && score > 0) {
                snprintf(message, sizeof(message), "NEW HIGH SCORE!\n\nSCORE: %" PRId32 "\nLENGTH: %" PRId32,
                        score, length);
            } else {
                snprintf(message, sizeof(message), "GAME OVER!\n\nSCORE: %" PRId32 "\nLENGTH: %" PRId32 "\nBEST: %" PRId32,
                        score, length, getHighScore(ctx, ctx->currentDifficulty));
            }
            const char* argv[] = { alertTitle, message, "OK" };
            app_manager_start_for_result("AlertDialog", ctx->appInstanceId, 3, argv, &ctx->gameOverDialogId);
        } else {
            // Update score display
            lv_label_set_text_fmt(ctx->scoreLabel, "SCORE: %u", snake_get_score(ctx->gameObject));
        }
    }
}

void newGameBtnEvent(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) {
        return;
    }
    snake_set_new_game(ctx->gameObject);
    // Update score label
    if (ctx->scoreLabel) {
        lv_label_set_text_fmt(ctx->scoreLabel, "SCORE: %u", snake_get_score(ctx->gameObject));
    }
}

void createGame(Context* ctx, lv_obj_t* parent, uint16_t cell_size, bool wallCollision, lv_obj_t* tb) {
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    // Create game widget
    ctx->gameObject = snake_create(parent, cell_size, wallCollision);
    if (!ctx->gameObject) {
        return;
    }
    lv_obj_set_size(ctx->gameObject, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_grow(ctx->gameObject, 1);

    // Create score wrapper in toolbar
    ctx->scoreWrapper = lv_obj_create(tb);
    lv_obj_set_size(ctx->scoreWrapper, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_pad_top(ctx->scoreWrapper, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(ctx->scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(ctx->scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(ctx->scoreWrapper, 10, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(ctx->scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(ctx->scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ctx->scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ctx->scoreWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(ctx->scoreWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Create score label
    ctx->scoreLabel = lv_label_create(ctx->scoreWrapper);
    lv_label_set_text_fmt(ctx->scoreLabel, "SCORE: %u", snake_get_score(ctx->gameObject));
    lv_obj_set_style_text_align(ctx->scoreLabel, LV_TEXT_ALIGN_LEFT, LV_STATE_DEFAULT);
    lv_obj_align(ctx->scoreLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(ctx->scoreLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(ctx->scoreLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(ctx->scoreLabel, lv_palette_main(LV_PALETTE_GREEN), LV_PART_MAIN);
    lv_obj_add_event_cb(ctx->gameObject, snakeEventCb, LV_EVENT_VALUE_CHANGED, ctx);

    auto ui_density = lvgl_get_ui_density();
    auto toolbar_height = getToolbarHeight(ui_density);
    auto icon_padding = getActionIconPadding(ui_density);

    // Create new game button wrapper
    ctx->newGameWrapper = lv_obj_create(tb);
    lv_obj_set_width(ctx->newGameWrapper, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->newGameWrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(ctx->newGameWrapper, icon_padding / 2, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(ctx->newGameWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(ctx->newGameWrapper, 0, LV_STATE_DEFAULT);

    // Create new game button
    lv_obj_t* newGameBtn = lv_btn_create(ctx->newGameWrapper);
    lv_obj_set_size(newGameBtn, toolbar_height - icon_padding, toolbar_height - icon_padding);
    lv_obj_set_style_pad_all(newGameBtn, 0, LV_STATE_DEFAULT);
    lv_obj_align(newGameBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(newGameBtn, newGameBtnEvent, LV_EVENT_CLICKED, ctx);

    lv_obj_t* btnIcon = lv_image_create(newGameBtn);
    lv_image_set_src(btnIcon, LV_SYMBOL_REFRESH);
    lv_obj_align(btnIcon, LV_ALIGN_CENTER, 0, 0);
}

} // namespace

void snakeCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    // Create toolbar
    ctx->toolbar = lvgl_toolbar_create(parent, "Snake");
    lv_obj_align(ctx->toolbar, LV_ALIGN_TOP_MID, 0, 0);

    // Create main wrapper
    ctx->mainWrapper = lv_obj_create(parent);
    lv_obj_set_width(ctx->mainWrapper, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->mainWrapper, 1);
    lv_obj_set_style_pad_all(ctx->mainWrapper, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_row(ctx->mainWrapper, 2, LV_PART_MAIN);
    lv_obj_set_style_pad_column(ctx->mainWrapper, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(ctx->mainWrapper, 0, LV_PART_MAIN);
    lv_obj_remove_flag(ctx->mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Load high scores on first build
    if (!ctx->highScoresLoaded) {
        loadHighScores(ctx);
        ctx->highScoresLoaded = true;
    }

    // Rebuild whatever was already committed. A game in progress survives a resurface caused by
    // burial from something other than our own dialogs (window_manager only ever destroys the
    // widget tree - Context's state, including currentDifficulty, is untouched) - snake_create()
    // owns all game state internally though, so there's no cheap way to resume the exact
    // position; this starts a fresh game at the same difficulty instead.
    //
    // Deliberately never opens a dialog from here - see Snake.h's comment on
    // snakeShowSelectionDialog()/snakeShowHelpDialog()/snakeStartGame()/snakeClearGame() for why:
    // this callback can run on a different app's thread mid-resurface, racing ahead of main()'s
    // own APP_EVENT_RESULT processing.
    if (ctx->currentDifficulty >= SNAKE_SELECTION_EASY && ctx->currentDifficulty <= SNAKE_SELECTION_HELL) {
        lv_obj_update_layout(parent);
        int32_t difficultyIndex = ctx->currentDifficulty - SNAKE_SELECTION_EASY;
        bool wallCollision = (ctx->currentDifficulty == SNAKE_SELECTION_HELL);
        createGame(ctx, ctx->mainWrapper, difficultySizes[difficultyIndex], wallCollision, ctx->toolbar);
    }
}

void snakeTeardown(Context* ctx) {
    ctx->scoreLabel = nullptr;
    ctx->scoreWrapper = nullptr;
    ctx->toolbar = nullptr;
    ctx->mainWrapper = nullptr;
    ctx->newGameWrapper = nullptr;
    ctx->gameObject = nullptr;
}

void snakeShowSelectionDialog(Context* ctx) {
    const char* argv[] = { "Snake", "How to Play", "Easy", "Medium", "Hard", "Hell" };
    app_manager_start_for_result("tactility.selectiondialog", ctx->appInstanceId, 6, argv, &ctx->selectionDialogId);
}

void snakeShowHelpDialog(Context* ctx) {
    const char* argv[] = {
        "How to Play",
        "Swipe or use arrow keys to change direction.\n"
        "Eat food to grow longer.\n"
        "Don't hit yourself!",
        "OK",
    };
    app_manager_start_for_result("tactility.alertdialog", ctx->appInstanceId, 3, argv, &ctx->helpDialogId);
}

void snakeClearGame(Context* ctx) {
    if (ctx->scoreWrapper) { lv_obj_delete(ctx->scoreWrapper); ctx->scoreWrapper = nullptr; }
    if (ctx->newGameWrapper) { lv_obj_delete(ctx->newGameWrapper); ctx->newGameWrapper = nullptr; }
    if (ctx->mainWrapper) lv_obj_clean(ctx->mainWrapper);
    ctx->scoreLabel = nullptr;
    ctx->gameObject = nullptr;
    ctx->currentDifficulty = -1;
}

void snakeStartGame(Context* ctx, int32_t difficulty) {
    if (!ctx->mainWrapper || !ctx->toolbar) return;
    snakeClearGame(ctx); // tear down any previous game's leftovers first (harmless if none)

    lv_obj_update_layout(ctx->mainWrapper);
    ctx->currentDifficulty = difficulty;
    int32_t difficultyIndex = difficulty - SNAKE_SELECTION_EASY;
    bool wallCollision = (difficulty == SNAKE_SELECTION_HELL);
    createGame(ctx, ctx->mainWrapper, difficultySizes[difficultyIndex], wallCollision, ctx->toolbar);
}
