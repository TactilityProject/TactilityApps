/**
 * @file TwoEleven.cpp
 * @brief 2048 game app implementation for Tactility
 */
#include "TwoEleven.h"

#include <inttypes.h>
#include <esp_log.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>
#include <app/manager.h>
#include <app/paths.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <tactility/preferences.h>
#include <string>

namespace {

constexpr size_t SIZE_COUNT = 4;

// Grid size options (index matches selection - 1)
constexpr uint16_t gridSizes[SIZE_COUNT] = { 3, 4, 5, 6 };

bool getPreferencesPath(std::string& outPath) {
    char path[128];
    if (app_paths_get_user_data_path("tactility.twoeleven", "two_eleven.properties", path, sizeof(path)) != ERROR_NONE) {
        return false;
    }
    outPath = std::string(path);
    return true;
}

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

void loadHighScores(Context* ctx) {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;
    preferences_opt_int32(prefs, "high_3x3", &ctx->highScore3x3);
    preferences_opt_int32(prefs, "high_4x4", &ctx->highScore4x4);
    preferences_opt_int32(prefs, "high_5x5", &ctx->highScore5x5);
    preferences_opt_int32(prefs, "high_6x6", &ctx->highScore6x6);
    preferences_close(prefs);
}

void saveHighScore(Context* ctx, int32_t gridSize, int32_t score) {
    std::string path;
    if (!getPreferencesPath(path)) return;
    Preferences* prefs = preferences_open(path.c_str());
    if (!prefs) return;
    switch (gridSize) {
        case TWOELEVEN_SELECTION_3X3:
            ctx->highScore3x3 = score;
            preferences_put_int32(prefs, "high_3x3", score);
            break;
        case TWOELEVEN_SELECTION_4X4:
            ctx->highScore4x4 = score;
            preferences_put_int32(prefs, "high_4x4", score);
            break;
        case TWOELEVEN_SELECTION_5X5:
            ctx->highScore5x5 = score;
            preferences_put_int32(prefs, "high_5x5", score);
            break;
        case TWOELEVEN_SELECTION_6X6:
            ctx->highScore6x6 = score;
            preferences_put_int32(prefs, "high_6x6", score);
            break;
    }
    preferences_close(prefs);
}

int32_t getHighScore(Context* ctx, int32_t gridSize) {
    switch (gridSize) {
        case TWOELEVEN_SELECTION_3X3: return ctx->highScore3x3;
        case TWOELEVEN_SELECTION_4X4: return ctx->highScore4x4;
        case TWOELEVEN_SELECTION_5X5: return ctx->highScore5x5;
        case TWOELEVEN_SELECTION_6X6: return ctx->highScore6x6;
        default: return 0;
    }
}

void twoElevenEventCb(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_VALUE_CHANGED) {
        int32_t score = twoeleven_get_score(ctx->gameObject);

        if (ctx->gameOverDialogId == 0 && twoeleven_get_best_tile(ctx->gameObject) >= 2048) {
            int32_t prevHighScore = getHighScore(ctx, ctx->currentGridSize);
            bool isNewHighScore = score > prevHighScore;

            // Save high score if it's a new record
            if (isNewHighScore) {
                saveHighScore(ctx, ctx->currentGridSize, score);
            }

            char message[100];
            const char* title = "YOU WIN!";
            if (isNewHighScore) {
                snprintf(message, sizeof(message), "NEW HIGH SCORE!\n\nSCORE: %" PRId32, score);
            } else {
                snprintf(message, sizeof(message), "YOU WIN!\n\nSCORE: %" PRId32 "\nBEST: %" PRId32, score, getHighScore(ctx, ctx->currentGridSize));
            }
            const char* argv[] = { title, message, "OK" };
            app_manager_start_for_result("tactility.alertdialog", ctx->appInstanceId, 3, argv, &ctx->gameOverDialogId);
        } else if (ctx->gameOverDialogId == 0 && twoeleven_get_status(ctx->gameObject)) {
            int32_t prevHighScore = getHighScore(ctx, ctx->currentGridSize);
            bool isNewHighScore = score > prevHighScore;

            // Save high score if it's a new record
            if (isNewHighScore) {
                saveHighScore(ctx, ctx->currentGridSize, score);
            }

            char message[100];
            const char* title;
            if (isNewHighScore && score > 0) {
                title = "NEW HIGH SCORE!";
                snprintf(message, sizeof(message), "NEW HIGH SCORE!\n\nSCORE: %" PRId32, score);
            } else {
                title = "GAME OVER!";
                snprintf(message, sizeof(message), "GAME OVER!\n\nSCORE: %" PRId32 "\nBEST: %" PRId32, score, getHighScore(ctx, ctx->currentGridSize));
            }
            const char* argv[] = { title, message, "OK" };
            app_manager_start_for_result("tactility.alertdialog", ctx->appInstanceId, 3, argv, &ctx->gameOverDialogId);
        } else {
            // Update score display
            lv_label_set_text_fmt(ctx->scoreLabel, "SCORE: %" PRId32, score);
        }
    }
}

void newGameBtnEvent(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    twoeleven_set_new_game(ctx->gameObject);
    // Update score label
    if (ctx->scoreLabel) {
        lv_label_set_text_fmt(ctx->scoreLabel, "SCORE: %" PRId32, twoeleven_get_score(ctx->gameObject));
    }
}

void createGame(Context* ctx, lv_obj_t* parent, uint16_t size, lv_obj_t* tb) {
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    // Create game widget
    ctx->gameObject = twoeleven_create(parent, size);
    lv_obj_set_style_text_font(ctx->gameObject, lv_font_get_default(), 0);
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
    lv_label_set_text_fmt(ctx->scoreLabel, "SCORE: %" PRId32, twoeleven_get_score(ctx->gameObject));
    lv_obj_set_style_text_align(ctx->scoreLabel, LV_TEXT_ALIGN_LEFT, LV_STATE_DEFAULT);
    lv_obj_align(ctx->scoreLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_size(ctx->scoreLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_text_font(ctx->scoreLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(ctx->scoreLabel, lv_palette_main(LV_PALETTE_AMBER), LV_PART_MAIN);
    lv_obj_add_event_cb(ctx->gameObject, twoElevenEventCb, LV_EVENT_VALUE_CHANGED, ctx);

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

void twoElevenCreateWidgets(lv_obj_t* parent, void* userData) {
    ESP_LOGI("TwoEleven", "twoElevenCreateWidgets called, parent=%p", parent);
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    // Create toolbar
    ctx->toolbar = lvgl_toolbar_create(parent, "2048");
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
    // widget tree - Context's state, including currentGridSize, is untouched) - twoeleven_create()
    // owns all game state internally though, so there's no cheap way to resume the exact
    // position; this starts a fresh game at the same grid size instead.
    //
    // Deliberately never opens a dialog from here - see TwoEleven.h's comment on
    // twoElevenShowSelectionDialog()/twoElevenShowHelpDialog()/twoElevenStartGame()/
    // twoElevenClearGame() for why: this callback can run on a different app's thread
    // mid-resurface, racing ahead of main()'s own APP_EVENT_RESULT processing.
    if (ctx->currentGridSize >= TWOELEVEN_SELECTION_3X3 && ctx->currentGridSize <= TWOELEVEN_SELECTION_6X6) {
        lv_obj_update_layout(parent);
        int32_t sizeIndex = ctx->currentGridSize - TWOELEVEN_SELECTION_3X3;
        createGame(ctx, ctx->mainWrapper, gridSizes[sizeIndex], ctx->toolbar);
    }
}

void twoElevenTeardown(Context* ctx) {
    ctx->scoreLabel = nullptr;
    ctx->scoreWrapper = nullptr;
    ctx->toolbar = nullptr;
    ctx->mainWrapper = nullptr;
    ctx->newGameWrapper = nullptr;
    ctx->gameObject = nullptr;
}

void twoElevenShowSelectionDialog(Context* ctx) {
    const char* argv[] = { "2048", "How to Play", "3x3", "4x4", "5x5", "6x6" };
    app_manager_start_for_result("tactility.selectiondialog", ctx->appInstanceId, 6, argv, &ctx->selectionDialogId);
}

void twoElevenShowHelpDialog(Context* ctx) {
    const char* argv[] = {
        "How to Play",
        "Swipe or use arrow keys to move tiles.\n"
        "Tiles with the same number merge.\n"
        "Reach 2048 to win!",
        "OK",
    };
    app_manager_start_for_result("tactility.alertdialog", ctx->appInstanceId, 3, argv, &ctx->helpDialogId);
}

void twoElevenClearGame(Context* ctx) {
    if (ctx->scoreWrapper) { lv_obj_delete(ctx->scoreWrapper); ctx->scoreWrapper = nullptr; }
    if (ctx->newGameWrapper) { lv_obj_delete(ctx->newGameWrapper); ctx->newGameWrapper = nullptr; }
    if (ctx->mainWrapper) lv_obj_clean(ctx->mainWrapper);
    ctx->scoreLabel = nullptr;
    ctx->gameObject = nullptr;
    ctx->currentGridSize = -1;
}

void twoElevenStartGame(Context* ctx, int32_t gridSize) {
    if (!ctx->mainWrapper || !ctx->toolbar) return;
    twoElevenClearGame(ctx); // tear down any previous game's leftovers first (harmless if none)

    lv_obj_update_layout(ctx->mainWrapper);
    ctx->currentGridSize = gridSize;
    int32_t sizeIndex = gridSize - TWOELEVEN_SELECTION_3X3;
    createGame(ctx, ctx->mainWrapper, gridSizes[sizeIndex], ctx->toolbar);
}
