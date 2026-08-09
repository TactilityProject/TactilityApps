/**
 * @file Breakout.cpp
 * @brief Breakout arcade game implementation for Tactility (Arkanoid-style)
 */
#include "Breakout.h"
#include "SfxEngine.h"

#include <cstdio>
#include <cmath>

#include <app/paths.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/preferences.h>

#include <esp_random.h>
#include <tactility/device.h>
#include <tactility/drivers/keyboard.h>

#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>

constexpr auto* TAG = "Breakout";

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "one.tactility.breakout";

static constexpr const char* PREF_HIGH_SCORE = "high";
static constexpr const char* PREF_SOUND = "sound";

// Persistent state (file-scope)
static int32_t highScore = 0;
static bool soundEnabled = true;

// Normal brick colors (8 colors, index 0-7)
static const lv_palette_t BRICK_COLORS[] = {
    LV_PALETTE_PURPLE,     // 50 Points
    LV_PALETTE_ORANGE,     // 60 Points
    LV_PALETTE_CYAN,       // 70 Points
    LV_PALETTE_GREEN,      // 80 Points
    LV_PALETTE_RED,        // 90 Points
    LV_PALETTE_BLUE,       // 100 Points
    LV_PALETTE_PINK,       // 110 Points
    LV_PALETTE_YELLOW      // 120 Points
};

// Power-up capsule colors and labels
static const uint32_t CAPSULE_COLORS[] = {
    0xFF0000,  // Laser - Red
    0x0088FF,  // Extend - Blue
    0x00CC00,  // Catch - Green
    0xFF8800,  // Slow - Orange
    0xFF44AA,  // BreakOut - Pink
    0x00DDDD,  // Split - Cyan
    0xAAAAAA   // ExtraLife - Grey
};
static const char* CAPSULE_LETTERS[] = { "L", "E", "C", "S", "B", "D", "+" };

// Capsule drop chance (out of 100)
static constexpr int CAPSULE_DROP_CHANCE = 15;
// Catch auto-release ticks (3 seconds at 40fps)
static constexpr int CATCH_AUTO_RELEASE_TICKS = 120;
// Slow recovery duration ticks (5 seconds)
static constexpr int SLOW_RECOVERY_TICKS = 200;
// Laser cooldown ticks between shots
static constexpr int LASER_COOLDOWN_TICKS = 12;
// Amber (Gold) bricks
static constexpr int INDESTRUCTIBLE_HITS = 999;

static bool getSettingsPath(char* buf, size_t bufSize) {
    return app_paths_get_user_data_path(APP_ID, "settings.properties", buf, bufSize) == ERROR_NONE;
}

static void loadSettings() {
    char path[192];
    if (!getSettingsPath(path, sizeof(path))) return;
    Preferences* prefs = preferences_open(path);
    if (prefs) {
        preferences_opt_int32(prefs, PREF_HIGH_SCORE, &highScore);
        int32_t snd = 1;
        preferences_opt_int32(prefs, PREF_SOUND, &snd);
        soundEnabled = (snd != 0);
        preferences_close(prefs);
    }
}

static void saveHighScore(int32_t score) {
    if (score <= highScore) return;
    highScore = score;
    char path[192];
    if (!getSettingsPath(path, sizeof(path))) return;
    Preferences* prefs = preferences_open(path);
    if (prefs) {
        preferences_put_int32(prefs, PREF_HIGH_SCORE, score);
        preferences_close(prefs);
    }
}

static void saveSoundSetting(bool enabled) {
    soundEnabled = enabled;
    char path[192];
    if (!getSettingsPath(path, sizeof(path))) return;
    Preferences* prefs = preferences_open(path);
    if (prefs) {
        preferences_put_int32(prefs, PREF_SOUND, enabled ? 1 : 0);
        preferences_close(prefs);
    }
}

// Simple seeded random for procedural levels
static uint32_t levelRng(uint32_t& seed) {
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) & 0x7FFF;
}

/* ── Forward declarations (game logic operates on Context*) ─────── */

static void startGame(Context* ctx);
static void nextLevel(Context* ctx);
static void resetBall(Context* ctx);
static void launchBall(Context* ctx);
static void update(Context* ctx);
static void checkLaserBrickCollisions(Context* ctx);
static void loseLife(Context* ctx);
static void winLevel(Context* ctx);
static void createBricks(Context* ctx);
static void setupLevelPattern(Context* ctx);
static void refreshBricks(Context* ctx);
static void updateScoreDisplay(Context* ctx);
static void updateMessage(Context* ctx);
static void togglePause(Context* ctx);
static void updateSoundIcon(Context* ctx);

static void spawnCapsule(Context* ctx, float x, float y);
static void updateCapsules(Context* ctx);
static void activatePowerUp(Context* ctx, PowerUpType type);
static void clearPowerUps(Context* ctx);
static void createCapsuleObjs(Context* ctx);

static void updateBalls(Context* ctx);
static void splitBalls(Context* ctx);

static void updateLasers(Context* ctx);
static void fireLaser(Context* ctx);
static void createLaserObjs(Context* ctx);

static void openExit(Context* ctx);
static void closeExit(Context* ctx);

static void hitBrick(Context* ctx, int idx);
static int scoreBrick(Context* ctx, int idx);

/* ── UI Creation ──────────────────────────────────────────────── */

static uint32_t getToolbarHeight(UiDensity uiDensity) {
    if (uiDensity == LVGL_UI_DENSITY_COMPACT) {
        return lvgl_get_text_font_height(FONT_SIZE_DEFAULT) * 1.4f;
    } else {
        return lvgl_get_text_font_height(FONT_SIZE_LARGE) * 2.2f;
    }
}

static uint32_t getActionIconPadding(UiDensity uiDensity) {
    auto toolbar_height = getToolbarHeight(uiDensity);
    return (uiDensity != LVGL_UI_DENSITY_COMPACT) ? (uint32_t)(toolbar_height * 0.2f) : 8;
}

/* ── Event Callbacks (declared here so createWidgets can wire them up) ── */

static void onTick(lv_timer_t* timer);
static void onPressed(lv_event_t* e);
static void onClicked(lv_event_t* e);
static void onKey(lv_event_t* e);
static void onReenterKeyMode(lv_event_t* e);
static void onPauseClicked(lv_event_t* e);
static void onSoundToggled(lv_event_t* e);

void breakoutCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    // Load settings on first show
    if (ctx->needsInit) loadSettings();

    // Start sfx engine
    if (!ctx->sfxEngine) {
        ctx->sfxEngine = new SfxEngine();
        ctx->sfxEngine->start();
        ctx->sfxEngine->setEnabled(soundEnabled);
    }

    // Toolbar
    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Breakout");

    // Score wrapper in toolbar
    lv_obj_t* scoreWrap = lv_obj_create(toolbar);
    lv_obj_set_size(scoreWrap, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_pad_top(scoreWrap, 4, 0);
    lv_obj_set_style_pad_bottom(scoreWrap, 0, 0);
    lv_obj_set_style_pad_left(scoreWrap, 0, 0);
    lv_obj_set_style_pad_right(scoreWrap, 10, 0);
    lv_obj_set_style_border_width(scoreWrap, 0, 0);
    lv_obj_set_style_bg_opa(scoreWrap, 0, 0);
    lv_obj_remove_flag(scoreWrap, LV_OBJ_FLAG_SCROLLABLE);

    ctx->scoreLabel = lv_label_create(scoreWrap);
    lv_obj_set_style_text_font(ctx->scoreLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(ctx->scoreLabel, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_align(ctx->scoreLabel, LV_ALIGN_CENTER, 0, 0);

    // Lives wrapper in toolbar
    lv_obj_t* livesWrap = lv_obj_create(toolbar);
    lv_obj_set_size(livesWrap, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_pad_top(livesWrap, 4, 0);
    lv_obj_set_style_pad_bottom(livesWrap, 0, 0);
    lv_obj_set_style_pad_left(livesWrap, 0, 0);
    lv_obj_set_style_pad_right(livesWrap, 10, 0);
    lv_obj_set_style_border_width(livesWrap, 0, 0);
    lv_obj_set_style_bg_opa(livesWrap, 0, 0);
    lv_obj_remove_flag(livesWrap, LV_OBJ_FLAG_SCROLLABLE);

    ctx->livesLabel = lv_label_create(livesWrap);
    lv_obj_set_style_text_font(ctx->livesLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(ctx->livesLabel, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_align(ctx->livesLabel, LV_ALIGN_CENTER, 0, 0);

    auto ui_density = lvgl_get_ui_density();
    auto toolbar_height = getToolbarHeight(ui_density);
    auto icon_padding = getActionIconPadding(ui_density);

    // Toolbar buttons wrapper
    lv_obj_t* btnsWrapper = lv_obj_create(toolbar);
    lv_obj_set_width(btnsWrapper, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnsWrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btnsWrapper, icon_padding / 2, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btnsWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btnsWrapper, 0, LV_STATE_DEFAULT);

    // Pause button
    lv_obj_t* pauseBtn = lv_btn_create(btnsWrapper);
    lv_obj_set_size(pauseBtn, toolbar_height - icon_padding, toolbar_height - icon_padding);
    lv_obj_set_style_pad_all(pauseBtn, 0, LV_STATE_DEFAULT);
    lv_obj_align(pauseBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(pauseBtn, onPauseClicked, LV_EVENT_CLICKED, ctx);

    lv_obj_t* pauseIcon = lv_label_create(pauseBtn);
    lv_label_set_text(pauseIcon, LV_SYMBOL_PAUSE);
    lv_obj_align(pauseIcon, LV_ALIGN_CENTER, 0, 0);

    // Sound toggle button
    lv_obj_t* soundBtn = lv_btn_create(btnsWrapper);
    lv_obj_set_size(soundBtn, toolbar_height - icon_padding, toolbar_height - icon_padding);
    lv_obj_set_style_pad_all(soundBtn, 0, LV_STATE_DEFAULT);
    lv_obj_align(soundBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(soundBtn, onSoundToggled, LV_EVENT_CLICKED, ctx);

    ctx->soundBtnIcon = lv_label_create(soundBtn);
    lv_obj_align(ctx->soundBtnIcon, LV_ALIGN_CENTER, 0, 0);
    updateSoundIcon(ctx);

    // Screen size detection (The Book)
    lv_coord_t screenW = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenH = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenW < 280 || screenH < 180);
    bool isXLarge = (screenW >= 600);

    // Scaled dimensions
    ctx->cols = isSmall ? 8 : (isXLarge ? 12 : 10);
    ctx->rows = isSmall ? 3 : (isXLarge ? 5 : 4);
    ctx->brickW = isSmall ? 24 : (isXLarge ? 56 : 28);
    ctx->brickH = isSmall ? 8 : (isXLarge ? 18 : 10);
    ctx->brickGap = isSmall ? 2 : (isXLarge ? 4 : 2);
    ctx->ballSize = isSmall ? 6 : (isXLarge ? 14 : 8);
    ctx->paddleW = isSmall ? 40 : (isXLarge ? 100 : 54);
    ctx->paddleH = isSmall ? 6 : (isXLarge ? 14 : 8);
    ctx->baseBallSpeed = isSmall ? 2.0f : (isXLarge ? 4.0f : 2.5f);
    ctx->paddleSpeed = isSmall ? 16.0f : (isXLarge ? 36.0f : 24.0f);
    int paddleMargin = isSmall ? 2 : (isXLarge ? 8 : 4);
    int brickTopPad = isSmall ? 4 : (isXLarge ? 12 : 8);

    // Capsule dimensions
    ctx->capsuleW = isSmall ? 16 : (isXLarge ? 36 : 22);
    ctx->capsuleH = isSmall ? 8 : (isXLarge ? 16 : 12);
    ctx->capsuleFallSpeed = isSmall ? 1.2f : (isXLarge ? 2.5f : 1.8f);

    // Laser dimensions
    ctx->laserW = isSmall ? 2 : (isXLarge ? 4 : 3);
    ctx->laserH = isSmall ? 6 : (isXLarge ? 12 : 8);
    ctx->laserSpeed = isSmall ? 4.0f : (isXLarge ? 8.0f : 6.0f);

    // Store original paddle width for extend reset
    ctx->originalPaddleW = ctx->paddleW;

    // Ball speed includes level scaling
    ctx->ballSpeed = ctx->baseBallSpeed + (ctx->level - 1) * 0.3f;

    // Game area
    ctx->gameArea = lv_obj_create(parent);
    lv_obj_set_width(ctx->gameArea, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->gameArea, 1);
    lv_obj_set_style_bg_color(ctx->gameArea, lv_color_hex(0x0a0a1e), 0);
    lv_obj_set_style_bg_opa(ctx->gameArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ctx->gameArea, 0, 0);
    lv_obj_set_style_pad_all(ctx->gameArea, 0, 0);
    lv_obj_set_style_radius(ctx->gameArea, 0, 0);
    lv_obj_remove_flag(ctx->gameArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ctx->gameArea, LV_OBJ_FLAG_CLICKABLE);

    // Force layout to get accurate game area dimensions
    lv_obj_update_layout(parent);
    ctx->areaW = lv_obj_get_content_width(ctx->gameArea);
    ctx->areaH = lv_obj_get_content_height(ctx->gameArea);
    ctx->paddleYPos = ctx->areaH - ctx->paddleH - paddleMargin;

    // Calculate brick layout (centered horizontally)
    int totalBrickW = ctx->cols * ctx->brickW + (ctx->cols - 1) * ctx->brickGap;
    ctx->brickOffsetX = (ctx->areaW - totalBrickW) / 2;
    ctx->brickOffsetY = brickTopPad;

    // Create bricks
    createBricks(ctx);

    // Create paddle
    ctx->paddle = lv_obj_create(ctx->gameArea);
    lv_obj_set_size(ctx->paddle, ctx->paddleW, ctx->paddleH);
    lv_obj_set_style_bg_color(ctx->paddle, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
    lv_obj_set_style_border_width(ctx->paddle, 0, 0);
    lv_obj_set_style_pad_all(ctx->paddle, 0, 0);
    lv_obj_set_style_radius(ctx->paddle, 2, 0);
    lv_obj_remove_flag(ctx->paddle, LV_OBJ_FLAG_SCROLLABLE);

    // Create balls (primary + extras for split)
    for (int i = 0; i < MAX_BALLS; i++) {
        ctx->balls[i].obj = lv_obj_create(ctx->gameArea);
        lv_obj_set_size(ctx->balls[i].obj, ctx->ballSize, ctx->ballSize);
        lv_obj_set_style_bg_color(ctx->balls[i].obj, lv_color_white(), 0);
        lv_obj_set_style_border_width(ctx->balls[i].obj, 0, 0);
        lv_obj_set_style_pad_all(ctx->balls[i].obj, 0, 0);
        lv_obj_set_style_radius(ctx->balls[i].obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(ctx->balls[i].obj, LV_OBJ_FLAG_SCROLLABLE);
        if (i > 0) {
            lv_obj_add_flag(ctx->balls[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Create capsule objects (pre-created, hidden)
    createCapsuleObjs(ctx);

    // Create laser objects (pre-created, hidden)
    createLaserObjs(ctx);

    // BreakOut exit indicator at paddle level (hidden by default)
    int exitH = ctx->paddleH * 3;
    ctx->exitIndicator = lv_obj_create(ctx->gameArea);
    lv_obj_set_size(ctx->exitIndicator, 6, exitH);
    lv_obj_set_style_bg_color(ctx->exitIndicator, lv_color_hex(0xFF44AA), 0);
    lv_obj_set_style_bg_opa(ctx->exitIndicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(ctx->exitIndicator, 0, 0);
    lv_obj_set_style_radius(ctx->exitIndicator, 0, 0);
    lv_obj_remove_flag(ctx->exitIndicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(ctx->exitIndicator, ctx->areaW - 6, ctx->paddleYPos - exitH / 2 + ctx->paddleH / 2);
    lv_obj_add_flag(ctx->exitIndicator, LV_OBJ_FLAG_HIDDEN);

    // Message overlay (centered in game area)
    ctx->messageLabel = lv_label_create(ctx->gameArea);
    lv_obj_set_style_text_color(ctx->messageLabel, lv_color_white(), 0);
    lv_obj_set_style_text_align(ctx->messageLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(ctx->messageLabel);

    // Initialize or restore
    if (ctx->needsInit) {
        ctx->paddleX = (ctx->areaW - ctx->paddleW) / 2.0f;
        startGame(ctx);
        ctx->needsInit = false;
    } else {
        // Restore visual positions from saved state
        // Re-apply extended paddle width if still active
        if (ctx->extendActive && ctx->paddle) lv_obj_set_width(ctx->paddle, ctx->paddleW);
        lv_obj_set_pos(ctx->paddle, (int)ctx->paddleX, ctx->paddleYPos);
        for (int i = 0; i < MAX_BALLS; i++) {
            if (ctx->balls[i].active && ctx->balls[i].obj) {
                lv_obj_set_pos(ctx->balls[i].obj, (int)ctx->balls[i].x, (int)ctx->balls[i].y);
                lv_obj_clear_flag(ctx->balls[i].obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
        // Restore falling capsules
        for (int i = 0; i < MAX_CAPSULES; i++) {
            if (ctx->capsules[i].active && ctx->capsuleObjs[i]) {
                lv_obj_set_pos(ctx->capsuleObjs[i], (int)ctx->capsules[i].x, (int)ctx->capsules[i].y);
                lv_obj_clear_flag(ctx->capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        // Restore exit indicator
        if (ctx->exitOpen && ctx->exitIndicator) lv_obj_clear_flag(ctx->exitIndicator, LV_OBJ_FLAG_HIDDEN);
        updateScoreDisplay(ctx);
        updateMessage(ctx);
    }

    // Input handlers
    lv_obj_add_event_cb(ctx->gameArea, onPressed, LV_EVENT_PRESSING, ctx);
    lv_obj_add_event_cb(ctx->gameArea, onClicked, LV_EVENT_SHORT_CLICKED, ctx);
    lv_obj_add_event_cb(ctx->gameArea, onKey, LV_EVENT_KEY, ctx);
    lv_obj_add_event_cb(ctx->gameArea, onReenterKeyMode, LV_EVENT_CLICKED, ctx);

    // Keyboard focus - explicit enter/exit, no focus/defocus handlers
    lv_group_t* group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, ctx->gameArea);
        lv_group_focus_obj(ctx->gameArea);
        lv_group_set_editing(group, true);
    }

    // Start game timer
    ctx->gameTimer = lv_timer_create(onTick, TICK_MS, ctx);
}

void breakoutTeardown(Context* ctx) {
    if (ctx->gameTimer) {
        lv_timer_delete(ctx->gameTimer);
        ctx->gameTimer = nullptr;
    }
    if (ctx->gameArea) {
        lv_group_t* group = lv_group_get_default();
        if (group) lv_group_set_editing(group, false);
        lv_group_remove_obj(ctx->gameArea);
    }
    ctx->gameArea = nullptr;
    ctx->paddle = nullptr;
    for (int i = 0; i < MAX_BRICKS; i++) ctx->bricks[i] = nullptr;
    for (int i = 0; i < MAX_BALLS; i++) ctx->balls[i].obj = nullptr;
    for (int i = 0; i < MAX_CAPSULES; i++) {
        ctx->capsuleObjs[i] = nullptr;
        ctx->capsuleLabels[i] = nullptr;
    }
    for (int i = 0; i < MAX_LASERS; i++) ctx->lasers[i].obj = nullptr;
    ctx->exitIndicator = nullptr;
    ctx->scoreLabel = nullptr;
    ctx->livesLabel = nullptr;
    ctx->messageLabel = nullptr;
    ctx->soundBtnIcon = nullptr;

    // Clean up sfx engine
    if (ctx->sfxEngine) {
        ctx->sfxEngine->stop();
        delete ctx->sfxEngine;
        ctx->sfxEngine = nullptr;
    }
}

/* ── Capsule & Laser Object Creation ─────────────────────────── */

static void createCapsuleObjs(Context* ctx) {
    for (int i = 0; i < MAX_CAPSULES; i++) {
        ctx->capsuleObjs[i] = lv_obj_create(ctx->gameArea);
        lv_obj_set_size(ctx->capsuleObjs[i], ctx->capsuleW, ctx->capsuleH);
        lv_obj_set_style_border_width(ctx->capsuleObjs[i], 1, 0);
        lv_obj_set_style_border_color(ctx->capsuleObjs[i], lv_color_white(), 0);
        lv_obj_set_style_pad_all(ctx->capsuleObjs[i], 0, 0);
        lv_obj_set_style_radius(ctx->capsuleObjs[i], 3, 0);
        lv_obj_remove_flag(ctx->capsuleObjs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(ctx->capsuleObjs[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(ctx->capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);

        ctx->capsuleLabels[i] = lv_label_create(ctx->capsuleObjs[i]);
        lv_obj_set_style_text_color(ctx->capsuleLabels[i], lv_color_white(), 0);
        lv_obj_center(ctx->capsuleLabels[i]);
    }
}

static void createLaserObjs(Context* ctx) {
    for (int i = 0; i < MAX_LASERS; i++) {
        ctx->lasers[i].obj = lv_obj_create(ctx->gameArea);
        lv_obj_set_size(ctx->lasers[i].obj, ctx->laserW, ctx->laserH);
        lv_obj_set_style_bg_color(ctx->lasers[i].obj, lv_color_hex(0xFF4444), 0);
        lv_obj_set_style_bg_opa(ctx->lasers[i].obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(ctx->lasers[i].obj, 0, 0);
        lv_obj_set_style_pad_all(ctx->lasers[i].obj, 0, 0);
        lv_obj_set_style_radius(ctx->lasers[i].obj, 0, 0);
        lv_obj_remove_flag(ctx->lasers[i].obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ctx->lasers[i].obj, LV_OBJ_FLAG_HIDDEN);
        ctx->lasers[i].active = false;
    }
}

/* ── Brick Creation ───────────────────────────────────────────── */

static void createBricks(Context* ctx) {
    for (int r = 0; r < ctx->rows; r++) {
        for (int c = 0; c < ctx->cols; c++) {
            int idx = r * ctx->cols + c;
            if (idx >= MAX_BRICKS) continue;
            ctx->bricks[idx] = lv_obj_create(ctx->gameArea);
            lv_obj_set_size(ctx->bricks[idx], ctx->brickW, ctx->brickH);
            lv_obj_set_style_border_width(ctx->bricks[idx], 0, 0);
            lv_obj_set_style_pad_all(ctx->bricks[idx], 0, 0);
            lv_obj_set_style_radius(ctx->bricks[idx], 2, 0);
            lv_obj_remove_flag(ctx->bricks[idx], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(ctx->bricks[idx], LV_OBJ_FLAG_CLICKABLE);

            int x = ctx->brickOffsetX + c * (ctx->brickW + ctx->brickGap);
            int y = ctx->brickOffsetY + r * (ctx->brickH + ctx->brickGap);
            lv_obj_set_pos(ctx->bricks[idx], x, y);
        }
    }
    refreshBricks(ctx);
}

static void setupLevelPattern(Context* ctx) {
    int total = ctx->cols * ctx->rows;
    int numPatterns = 12;

    // Initialize all bricks
    for (int i = 0; i < total; i++) {
        ctx->brickAlive[i] = true;
        ctx->brickType[i] = BrickType::Normal;
        ctx->brickHits[i] = 1;
    }

    int colorOffset = (ctx->level - 1) % 8;
    for (int r = 0; r < ctx->rows; r++) {
        for (int c = 0; c < ctx->cols; c++) {
            ctx->brickColorIndex[r * ctx->cols + c] = (r + colorOffset) % 8;
        }
    }

    if (ctx->level <= numPatterns) {
        // Hardcoded patterns
        int pattern = (ctx->level - 1) % numPatterns;
        int rows = ctx->rows;
        int cols = ctx->cols;
        bool* brickAlive = ctx->brickAlive;
        switch (pattern) {
            case 0: // Full grid
                break;
            case 1: // Checkerboard
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c++)
                        if ((r + c) % 2 != 0) brickAlive[r * cols + c] = false;
                break;
            case 2: // Diamond
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c++) {
                        int dr = std::abs(r - rows / 2);
                        int dc = std::abs(c - cols / 2);
                        if (dr + dc > (rows / 2 + cols / 4)) brickAlive[r * cols + c] = false;
                    }
                break;
            case 3: // Horizontal stripes
                for (int r = 0; r < rows; r++)
                    if (r % 2 != 0)
                        for (int c = 0; c < cols; c++) brickAlive[r * cols + c] = false;
                break;
            case 4: // Pyramid (wider at top, narrows down)
                for (int r = 0; r < rows; r++) {
                    int margin = r;
                    for (int c = 0; c < cols; c++)
                        if (c < margin || c >= cols - margin) brickAlive[r * cols + c] = false;
                }
                break;
            case 5: // Inverted V (wider at bottom)
                for (int r = 0; r < rows; r++) {
                    int margin = rows - 1 - r;
                    for (int c = 0; c < cols; c++)
                        if (c < margin || c >= cols - margin) brickAlive[r * cols + c] = false;
                }
                break;
            case 6: // Vertical stripes
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c++)
                        if (c % 2 != 0) brickAlive[r * cols + c] = false;
                break;
            case 7: // Zigzag rows
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c++) {
                        int shift = (r % 2 == 0) ? 0 : 2;
                        if ((c + shift) % 4 >= 2) brickAlive[r * cols + c] = false;
                    }
                break;
            case 8: // Alternating blocks (2x2 groups)
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c++) {
                        int br = r / 2;
                        int bc = c / 2;
                        if ((br + bc) % 2 != 0) brickAlive[r * cols + c] = false;
                    }
                break;
            case 9: // Double diamond
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c++) {
                        int dr = std::abs(r - rows / 2);
                        int leftC = cols / 4;
                        int rightC = cols * 3 / 4;
                        int dcLeft = std::abs(c - leftC);
                        int dcRight = std::abs(c - rightC);
                        int minDc = dcLeft < dcRight ? dcLeft : dcRight;
                        if (dr + minDc > (rows / 2 + 1)) brickAlive[r * cols + c] = false;
                    }
                break;
            case 10: // Border frame (sparser - saved for later levels)
                for (int r = 1; r < rows - 1; r++)
                    for (int c = 1; c < cols - 1; c++)
                        brickAlive[r * cols + c] = false;
                break;
            case 11: // Center cross
                for (int r = 0; r < rows; r++)
                    for (int c = 0; c < cols; c++) {
                        bool onHoriz = (r == rows / 2);
                        bool onVert = (c == cols / 2 || c == cols / 2 - 1);
                        if (!onHoriz && !onVert) brickAlive[r * cols + c] = false;
                    }
                break;
        }
    } else {
        // Procedural generation (levels 13+)
        uint32_t seed = (uint32_t)ctx->level * 2654435761u;
        int density = 50 + (ctx->level * 2);
        if (density > 85) density = 85;

        for (int i = 0; i < total; i++) {
            int roll = (int)(levelRng(seed) % 100);
            ctx->brickAlive[i] = (roll < density);
        }
        // Ensure at least some bricks exist
        int aliveCount = 0;
        for (int i = 0; i < total; i++) if (ctx->brickAlive[i]) aliveCount++;
        if (aliveCount < 5) {
            for (int i = 0; i < total && aliveCount < 8; i++) {
                if (!ctx->brickAlive[i]) { ctx->brickAlive[i] = true; aliveCount++; }
            }
        }
    }

    // Add Silver bricks (level 3+)
    if (ctx->level >= 3) {
        int silverCount = (ctx->level - 2);
        if (silverCount > ctx->rows) silverCount = ctx->rows;
        int silverHits = (ctx->level >= 7) ? 3 : 2;
        uint32_t seed = (uint32_t)ctx->level * 31337u;
        int placed = 0;
        for (int attempt = 0; attempt < total * 2 && placed < silverCount; attempt++) {
            int idx = (int)(levelRng(seed) % total);
            if (ctx->brickAlive[idx] && ctx->brickType[idx] == BrickType::Normal) {
                ctx->brickType[idx] = BrickType::Silver;
                ctx->brickHits[idx] = silverHits;
                placed++;
            }
        }
    }

    // Add Gold bricks (level 5+)
    if (ctx->level >= 5) {
        int goldCount = (ctx->level - 4) / 2;
        if (goldCount > 4) goldCount = 4;
        uint32_t seed = (uint32_t)ctx->level * 48271u;
        int placed = 0;
        for (int attempt = 0; attempt < total * 2 && placed < goldCount; attempt++) {
            int idx = (int)(levelRng(seed) % total);
            if (ctx->brickAlive[idx] && ctx->brickType[idx] == BrickType::Normal) {
                ctx->brickType[idx] = BrickType::Gold;
                ctx->brickHits[idx] = INDESTRUCTIBLE_HITS; // indestructible
                placed++;
            }
        }
    }

    // Count alive bricks (excluding Gold)
    ctx->bricksRemaining = 0;
    ctx->destroyedCount = 0;
    for (int i = 0; i < total; i++) {
        if (ctx->brickAlive[i] && ctx->brickType[i] != BrickType::Gold) ctx->bricksRemaining++;
    }
}

static void refreshBricks(Context* ctx) {
    for (int r = 0; r < ctx->rows; r++) {
        for (int c = 0; c < ctx->cols; c++) {
            int idx = r * ctx->cols + c;
            if (!ctx->bricks[idx]) continue;

            if (!ctx->brickAlive[idx]) {
                lv_obj_add_flag(ctx->bricks[idx], LV_OBJ_FLAG_HIDDEN);
                continue;
            }

            lv_obj_clear_flag(ctx->bricks[idx], LV_OBJ_FLAG_HIDDEN);

            switch (ctx->brickType[idx]) {
                case BrickType::Normal: {
                    lv_palette_t color = BRICK_COLORS[ctx->brickColorIndex[idx]];
                    lv_obj_set_style_bg_color(ctx->bricks[idx], lv_palette_main(color), 0);
                    lv_obj_set_style_border_width(ctx->bricks[idx], 0, 0);
                    break;
                }
                case BrickType::Silver: {
                    int darken = 3 - ctx->brickHits[idx]; // more hits taken = darker
                    if (darken < 0) darken = 0;
                    if (darken > 3) darken = 3;
                    lv_obj_set_style_bg_color(ctx->bricks[idx], lv_palette_darken(LV_PALETTE_GREY, darken), 0);
                    lv_obj_set_style_border_width(ctx->bricks[idx], 1, 0);
                    lv_obj_set_style_border_color(ctx->bricks[idx], lv_color_white(), 0);
                    break;
                }
                case BrickType::Gold:
                    lv_obj_set_style_bg_color(ctx->bricks[idx], lv_palette_main(LV_PALETTE_AMBER), 0);
                    lv_obj_set_style_border_width(ctx->bricks[idx], 1, 0);
                    lv_obj_set_style_border_color(ctx->bricks[idx], lv_palette_lighten(LV_PALETTE_AMBER, 2), 0);
                    break;
            }
        }
    }
}

/* ── Brick Hit Logic ──────────────────────────────────────────── */

static int scoreBrick(Context* ctx, int idx) {
    switch (ctx->brickType[idx]) {
        case BrickType::Silver:
            return 50 * ctx->level;
        case BrickType::Gold:
            return 0; // can't be destroyed
        case BrickType::Normal:
        default:
            return COLOR_SCORES[ctx->brickColorIndex[idx] % 8];
    }
}

static void hitBrick(Context* ctx, int idx) {
    if (!ctx->brickAlive[idx]) return;

    if (ctx->brickType[idx] == BrickType::Gold) {
        // Bounce but don't damage
        if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Click);
        return;
    }

    ctx->brickHits[idx]--;
    if (ctx->brickHits[idx] <= 0) {
        // Brick destroyed
        ctx->brickAlive[idx] = false;
        if (ctx->bricks[idx]) lv_obj_add_flag(ctx->bricks[idx], LV_OBJ_FLAG_HIDDEN);
        ctx->score += scoreBrick(ctx, idx);
        if (ctx->brickType[idx] != BrickType::Gold) {
            ctx->bricksRemaining--;
            ctx->destroyedCount++;
        }

        // Speed up ball slightly every 5 bricks destroyed
        if (ctx->destroyedCount % 5 == 0) {
            ctx->ballSpeed += 0.15f;
            // Scale active ball velocities
            for (int b = 0; b < MAX_BALLS; b++) {
                if (!ctx->balls[b].active) continue;
                float curSpd = std::sqrt(ctx->balls[b].vx * ctx->balls[b].vx + ctx->balls[b].vy * ctx->balls[b].vy);
                if (curSpd > 0.01f) {
                    float scale = ctx->ballSpeed / curSpd;
                    ctx->balls[b].vx *= scale;
                    ctx->balls[b].vy *= scale;
                }
            }
        }

        updateScoreDisplay(ctx);
        if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::BrickHit);

        // Try to spawn capsule (only when single ball)
        if (ctx->activeBallCount <= 1) {
            int bx = ctx->brickOffsetX + (idx % ctx->cols) * (ctx->brickW + ctx->brickGap);
            int by = ctx->brickOffsetY + (idx / ctx->cols) * (ctx->brickH + ctx->brickGap);
            if ((int)(esp_random() % 100) < CAPSULE_DROP_CHANCE) {
                spawnCapsule(ctx, (float)bx, (float)by);
            }
        }

        if (ctx->bricksRemaining <= 0) {
            winLevel(ctx);
        }
    } else {
        // Multi-hit brick took damage (Silver)
        if (ctx->bricks[idx]) {
            int darken = 3 - ctx->brickHits[idx];
            if (darken < 0) darken = 0;
            if (darken > 3) darken = 3;
            lv_obj_set_style_bg_color(ctx->bricks[idx], lv_palette_darken(LV_PALETTE_GREY, darken), 0);
        }
        if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Click);
    }
}

/* ── Game State Management ────────────────────────────────────── */

static void startGame(Context* ctx) {
    ctx->score = 0;
    ctx->lives = INITIAL_LIVES;
    ctx->level = 1;
    ctx->state = GameState::Ready;
    ctx->ballSpeed = ctx->baseBallSpeed;

    clearPowerUps(ctx);
    setupLevelPattern(ctx);
    refreshBricks(ctx);

    ctx->paddleX = (ctx->areaW - ctx->paddleW) / 2.0f;
    if (ctx->paddle) lv_obj_set_pos(ctx->paddle, (int)ctx->paddleX, ctx->paddleYPos);

    resetBall(ctx);
    updateScoreDisplay(ctx);
    updateMessage(ctx);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Confirm);
}

static void nextLevel(Context* ctx) {
    ctx->level++;

    // Speed up ball each level
    ctx->ballSpeed = ctx->baseBallSpeed + (ctx->level - 1) * 0.3f;

    clearPowerUps(ctx);
    setupLevelPattern(ctx);
    refreshBricks(ctx);

    ctx->state = GameState::Ready;
    ctx->paddleX = (ctx->areaW - ctx->paddleW) / 2.0f;
    if (ctx->paddle) lv_obj_set_pos(ctx->paddle, (int)ctx->paddleX, ctx->paddleYPos);

    resetBall(ctx);
    updateScoreDisplay(ctx);
    updateMessage(ctx);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::LevelUp);
}

static void resetBall(Context* ctx) {
    // Reset to single ball
    for (int i = 1; i < MAX_BALLS; i++) {
        ctx->balls[i].active = false;
        if (ctx->balls[i].obj) lv_obj_add_flag(ctx->balls[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
    ctx->activeBallCount = 1;

    ctx->balls[0].active = true;
    ctx->balls[0].x = ctx->paddleX + ctx->paddleW / 2.0f - ctx->ballSize / 2.0f;
    ctx->balls[0].y = (float)(ctx->paddleYPos - ctx->ballSize - 2);
    ctx->balls[0].vx = 0;
    ctx->balls[0].vy = 0;
    if (ctx->balls[0].obj) {
        lv_obj_set_pos(ctx->balls[0].obj, (int)ctx->balls[0].x, (int)ctx->balls[0].y);
        lv_obj_clear_flag(ctx->balls[0].obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void launchBall(Context* ctx) {
    if (ctx->catchActive && ctx->catchBallIndex >= 0) {
        // Release caught ball
        BallState& b = ctx->balls[ctx->catchBallIndex];
        b.vx = (esp_random() % 2 ? 1.0f : -1.0f) * ctx->ballSpeed * 0.7f;
        b.vy = -ctx->ballSpeed;
        ctx->catchBallIndex = -1;
        ctx->catchAutoReleaseTicks = 0;
        if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Confirm);
        return;
    }

    ctx->balls[0].vx = (esp_random() % 2 ? 1.0f : -1.0f) * ctx->ballSpeed * 0.7f;
    ctx->balls[0].vy = -ctx->ballSpeed;
    ctx->state = GameState::Playing;
    updateMessage(ctx);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Confirm);
}

static void loseLife(Context* ctx) {
    ctx->lives--;
    clearPowerUps(ctx);

    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Hurt);

    if (ctx->lives <= 0) {
        ctx->state = GameState::GameOver;
        for (int i = 0; i < MAX_BALLS; i++) {
            ctx->balls[i].vx = 0;
            ctx->balls[i].vy = 0;
        }
        if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::GameOver);
    } else {
        ctx->state = GameState::Ready;
        ctx->paddleX = (ctx->areaW - ctx->paddleW) / 2.0f;
        if (ctx->paddle) lv_obj_set_pos(ctx->paddle, (int)ctx->paddleX, ctx->paddleYPos);
        resetBall(ctx);
    }
    updateScoreDisplay(ctx);
    updateMessage(ctx);
    if (ctx->lives <= 0) {
        saveHighScore(ctx->score);
    }
}

static void winLevel(Context* ctx) {
    saveHighScore(ctx->score);
    nextLevel(ctx);
}

static void togglePause(Context* ctx) {
    if (ctx->state == GameState::Playing) {
        ctx->state = GameState::Paused;
        updateMessage(ctx);
    } else if (ctx->state == GameState::Paused) {
        ctx->state = GameState::Playing;
        updateMessage(ctx);
    }
}

/* ── Power-Up System ──────────────────────────────────────────── */

static void spawnCapsule(Context* ctx, float x, float y) {
    for (int i = 0; i < MAX_CAPSULES; i++) {
        if (!ctx->capsules[i].active) {
            ctx->capsules[i].active = true;
            ctx->capsules[i].x = x;
            ctx->capsules[i].y = y;

            // Random power-up type (ExtraLife rarer)
            int roll = (int)(esp_random() % 100);
            if (roll < 5) {
                ctx->capsules[i].type = PowerUpType::ExtraLife;
            } else if (roll < 18) {
                ctx->capsules[i].type = PowerUpType::Laser;
            } else if (roll < 33) {
                ctx->capsules[i].type = PowerUpType::Extend;
            } else if (roll < 48) {
                ctx->capsules[i].type = PowerUpType::Catch;
            } else if (roll < 63) {
                ctx->capsules[i].type = PowerUpType::Slow;
            } else if (roll < 78) {
                ctx->capsules[i].type = PowerUpType::Split;
            } else {
                ctx->capsules[i].type = PowerUpType::BreakOut;
            }

            int typeIdx = static_cast<int>(ctx->capsules[i].type);
            if (ctx->capsuleObjs[i]) {
                lv_obj_set_style_bg_color(ctx->capsuleObjs[i], lv_color_hex(CAPSULE_COLORS[typeIdx]), 0);
                lv_obj_set_pos(ctx->capsuleObjs[i], (int)x, (int)y);
                lv_obj_clear_flag(ctx->capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (ctx->capsuleLabels[i]) {
                lv_label_set_text(ctx->capsuleLabels[i], CAPSULE_LETTERS[typeIdx]);
            }
            return;
        }
    }
}

static void updateCapsules(Context* ctx) {
    for (int i = 0; i < MAX_CAPSULES; i++) {
        if (!ctx->capsules[i].active) continue;

        ctx->capsules[i].y += ctx->capsuleFallSpeed;

        // Off screen
        if (ctx->capsules[i].y > ctx->areaH) {
            ctx->capsules[i].active = false;
            if (ctx->capsuleObjs[i]) lv_obj_add_flag(ctx->capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        // Paddle collision
        if (ctx->capsules[i].y + ctx->capsuleH > ctx->paddleYPos &&
            ctx->capsules[i].y < ctx->paddleYPos + ctx->paddleH &&
            ctx->capsules[i].x + ctx->capsuleW > ctx->paddleX &&
            ctx->capsules[i].x < ctx->paddleX + ctx->paddleW) {
            activatePowerUp(ctx, ctx->capsules[i].type);
            ctx->capsules[i].active = false;
            if (ctx->capsuleObjs[i]) lv_obj_add_flag(ctx->capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        if (ctx->capsuleObjs[i]) lv_obj_set_pos(ctx->capsuleObjs[i], (int)ctx->capsules[i].x, (int)ctx->capsules[i].y);
    }
}

static void activatePowerUp(Context* ctx, PowerUpType type) {
    if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Powerup);

    switch (type) {
        case PowerUpType::Extend:
            if (!ctx->extendActive) {
                ctx->extendActive = true;
                ctx->paddleW = (int)(ctx->originalPaddleW * 1.5f);
                int maxW = ctx->areaW / 2;
                if (ctx->paddleW > maxW) ctx->paddleW = maxW;
                if (ctx->paddle) lv_obj_set_width(ctx->paddle, ctx->paddleW);
                // Re-clamp paddle position
                if (ctx->paddleX + ctx->paddleW > ctx->areaW) ctx->paddleX = (float)(ctx->areaW - ctx->paddleW);
                if (ctx->paddle) lv_obj_set_x(ctx->paddle, (int)ctx->paddleX);
            }
            break;

        case PowerUpType::ExtraLife:
            ctx->lives++;
            if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::OneUp);
            updateScoreDisplay(ctx);
            break;

        case PowerUpType::Slow:
            if (ctx->slowRecoveryTicks <= 0) {
                // First slow: save speed and apply reduction
                ctx->originalBallSpeed = ctx->ballSpeed;
                ctx->ballSpeed *= 0.6f;
                // Scale all active ball velocities
                for (int b = 0; b < MAX_BALLS; b++) {
                    if (!ctx->balls[b].active) continue;
                    float curSpd = std::sqrt(ctx->balls[b].vx * ctx->balls[b].vx + ctx->balls[b].vy * ctx->balls[b].vy);
                    if (curSpd > 0.01f) {
                        float scale = ctx->ballSpeed / curSpd;
                        ctx->balls[b].vx *= scale;
                        ctx->balls[b].vy *= scale;
                    }
                }
            }
            // Reset (or extend) recovery timer
            ctx->slowRecoveryTicks = SLOW_RECOVERY_TICKS;
            break;

        case PowerUpType::Catch:
            ctx->catchActive = true;
            ctx->catchBallIndex = -1;
            break;

        case PowerUpType::Split:
            splitBalls(ctx);
            break;

        case PowerUpType::Laser:
            ctx->laserActive = true;
            ctx->laserCooldown = 0;
            break;

        case PowerUpType::BreakOut:
            openExit(ctx);
            break;
    }
}

static void clearPowerUps(Context* ctx) {
    // Reset extend
    if (ctx->extendActive) {
        ctx->extendActive = false;
        ctx->paddleW = ctx->originalPaddleW;
        if (ctx->paddle) lv_obj_set_width(ctx->paddle, ctx->paddleW);
    }

    // Reset catch
    ctx->catchActive = false;
    ctx->catchBallIndex = -1;
    ctx->catchAutoReleaseTicks = 0;

    // Reset slow
    if (ctx->slowRecoveryTicks > 0) {
        ctx->ballSpeed = ctx->baseBallSpeed + (ctx->level - 1) * 0.3f;
        ctx->slowRecoveryTicks = 0;
    }

    // Reset laser
    ctx->laserActive = false;
    ctx->laserCooldown = 0;
    for (int i = 0; i < MAX_LASERS; i++) {
        ctx->lasers[i].active = false;
        if (ctx->lasers[i].obj) lv_obj_add_flag(ctx->lasers[i].obj, LV_OBJ_FLAG_HIDDEN);
    }

    // Clear capsules
    for (int i = 0; i < MAX_CAPSULES; i++) {
        ctx->capsules[i].active = false;
        if (ctx->capsuleObjs[i]) lv_obj_add_flag(ctx->capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Close exit
    closeExit(ctx);
}

/* ── Multi-Ball ───────────────────────────────────────────────── */

static void splitBalls(Context* ctx) {
    // Find first active ball to split from
    int sourceIdx = -1;
    for (int i = 0; i < MAX_BALLS; i++) {
        if (ctx->balls[i].active) { sourceIdx = i; break; }
    }
    if (sourceIdx < 0) return;

    BallState& src = ctx->balls[sourceIdx];
    int spawned = 0;
    for (int i = 0; i < MAX_BALLS && spawned < 2; i++) {
        if (ctx->balls[i].active) continue;
        ctx->balls[i].active = true;
        ctx->balls[i].x = src.x;
        ctx->balls[i].y = src.y;

        // Diverging angles: +30 and -30 degrees from source
        float angle = (spawned == 0) ? 0.5f : -0.5f;
        float speed = std::sqrt(src.vx * src.vx + src.vy * src.vy);
        if (speed < 0.01f) speed = ctx->ballSpeed;
        float srcAngle = std::atan2(src.vy, src.vx);
        ctx->balls[i].vx = speed * std::cos(srcAngle + angle);
        ctx->balls[i].vy = speed * std::sin(srcAngle + angle);

        if (ctx->balls[i].obj) {
            lv_obj_set_pos(ctx->balls[i].obj, (int)ctx->balls[i].x, (int)ctx->balls[i].y);
            lv_obj_clear_flag(ctx->balls[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
        spawned++;
    }
    ctx->activeBallCount += spawned;
}

static void updateBalls(Context* ctx) {
    for (int b = 0; b < MAX_BALLS; b++) {
        if (!ctx->balls[b].active) continue;

        // Caught ball follows paddle
        if (ctx->catchActive && ctx->catchBallIndex == b) {
            ctx->balls[b].x = ctx->paddleX + ctx->catchOffsetX;
            ctx->balls[b].y = (float)(ctx->paddleYPos - ctx->ballSize - 2);
            if (ctx->balls[b].obj) lv_obj_set_pos(ctx->balls[b].obj, (int)ctx->balls[b].x, (int)ctx->balls[b].y);

            ctx->catchAutoReleaseTicks++;
            if (ctx->catchAutoReleaseTicks >= CATCH_AUTO_RELEASE_TICKS) {
                // Auto-release
                ctx->balls[b].vx = (esp_random() % 2 ? 1.0f : -1.0f) * ctx->ballSpeed * 0.7f;
                ctx->balls[b].vy = -ctx->ballSpeed;
                ctx->catchBallIndex = -1;
                ctx->catchAutoReleaseTicks = 0;
            }
            continue;
        }

        // Move ball
        ctx->balls[b].x += ctx->balls[b].vx;
        ctx->balls[b].y += ctx->balls[b].vy;

        // Left wall collision
        if (ctx->balls[b].x < 0) {
            ctx->balls[b].x = 0;
            ctx->balls[b].vx = -ctx->balls[b].vx;
            if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Click);
        }

        // Right wall collision (ball always bounces, exit is paddle-only)
        if (ctx->balls[b].x + ctx->ballSize > ctx->areaW) {
            ctx->balls[b].x = (float)(ctx->areaW - ctx->ballSize);
            ctx->balls[b].vx = -ctx->balls[b].vx;
            if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Click);
        }

        // Top wall
        if (ctx->balls[b].y < 0) {
            ctx->balls[b].y = 0;
            ctx->balls[b].vy = -ctx->balls[b].vy;
            if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Click);
        }

        // Bottom edge
        if (ctx->balls[b].y + ctx->ballSize > ctx->areaH) {
            ctx->balls[b].active = false;
            if (ctx->balls[b].obj) lv_obj_add_flag(ctx->balls[b].obj, LV_OBJ_FLAG_HIDDEN);
            ctx->activeBallCount--;
            if (ctx->activeBallCount <= 0) {
                ctx->activeBallCount = 0;
                loseLife(ctx);
                return;
            }
            continue;
        }

        // Paddle collision
        if (ctx->balls[b].vy > 0 &&
            ctx->balls[b].x + ctx->ballSize > ctx->paddleX && ctx->balls[b].x < ctx->paddleX + ctx->paddleW &&
            ctx->balls[b].y + ctx->ballSize > ctx->paddleYPos && ctx->balls[b].y < ctx->paddleYPos + ctx->paddleH) {

            if (ctx->catchActive && ctx->catchBallIndex < 0) {
                // Catch the ball
                ctx->catchBallIndex = b;
                ctx->catchOffsetX = ctx->balls[b].x - ctx->paddleX;
                ctx->catchAutoReleaseTicks = 0;
                ctx->balls[b].vx = 0;
                ctx->balls[b].vy = 0;
                ctx->balls[b].y = (float)(ctx->paddleYPos - ctx->ballSize - 2);
                if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Click);
            } else {
                // Normal bounce
                float hitPos = (ctx->balls[b].x + ctx->ballSize / 2.0f - ctx->paddleX) / (float)ctx->paddleW;
                float angle = (hitPos - 0.5f) * 2.0f;
                ctx->balls[b].vx = angle * ctx->ballSpeed;
                ctx->balls[b].vy = -std::fabs(ctx->balls[b].vy);
                if (std::fabs(ctx->balls[b].vy) < ctx->ballSpeed * 0.3f) {
                    ctx->balls[b].vy = -ctx->ballSpeed * 0.3f;
                }
                ctx->balls[b].y = (float)(ctx->paddleYPos - ctx->ballSize);
                if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Click);
            }
        }

        // Brick collisions for this ball
        for (int r = 0; r < ctx->rows; r++) {
            for (int c = 0; c < ctx->cols; c++) {
                int idx = r * ctx->cols + c;
                if (!ctx->brickAlive[idx]) continue;

                float bx = (float)(ctx->brickOffsetX + c * (ctx->brickW + ctx->brickGap));
                float by = (float)(ctx->brickOffsetY + r * (ctx->brickH + ctx->brickGap));

                if (ctx->balls[b].x + ctx->ballSize > bx && ctx->balls[b].x < bx + ctx->brickW &&
                    ctx->balls[b].y + ctx->ballSize > by && ctx->balls[b].y < by + ctx->brickH) {

                    hitBrick(ctx, idx);

                    // Bounce direction
                    float overlapLeft = ctx->balls[b].x + ctx->ballSize - bx;
                    float overlapRight = bx + ctx->brickW - ctx->balls[b].x;
                    float overlapTop = ctx->balls[b].y + ctx->ballSize - by;
                    float overlapBottom = by + ctx->brickH - ctx->balls[b].y;
                    float minOverlapX = overlapLeft < overlapRight ? overlapLeft : overlapRight;
                    float minOverlapY = overlapTop < overlapBottom ? overlapTop : overlapBottom;

                    if (minOverlapX < minOverlapY) {
                        ctx->balls[b].vx = -ctx->balls[b].vx;
                    } else {
                        ctx->balls[b].vy = -ctx->balls[b].vy;
                    }

                    goto nextBall; // One brick per ball per tick
                }
            }
        }

        nextBall:
        if (ctx->balls[b].obj && ctx->balls[b].active) {
            lv_obj_set_pos(ctx->balls[b].obj, (int)ctx->balls[b].x, (int)ctx->balls[b].y);
        }
    }
}

/* ── Laser System ─────────────────────────────────────────────── */

static void fireLaser(Context* ctx) {
    for (int i = 0; i < MAX_LASERS; i++) {
        if (!ctx->lasers[i].active) {
            ctx->lasers[i].active = true;
            ctx->lasers[i].x = ctx->paddleX + ctx->paddleW / 2.0f - ctx->laserW / 2.0f;
            ctx->lasers[i].y = (float)(ctx->paddleYPos - ctx->laserH);
            if (ctx->lasers[i].obj) {
                lv_obj_set_pos(ctx->lasers[i].obj, (int)ctx->lasers[i].x, (int)ctx->lasers[i].y);
                lv_obj_clear_flag(ctx->lasers[i].obj, LV_OBJ_FLAG_HIDDEN);
            }
            if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Laser);
            return;
        }
    }
}

static void updateLasers(Context* ctx) {
    if (!ctx->laserActive) return;

    // Auto-fire
    ctx->laserCooldown--;
    if (ctx->laserCooldown <= 0) {
        fireLaser(ctx);
        ctx->laserCooldown = LASER_COOLDOWN_TICKS;
    }

    for (int i = 0; i < MAX_LASERS; i++) {
        if (!ctx->lasers[i].active) continue;

        ctx->lasers[i].y -= ctx->laserSpeed;

        if (ctx->lasers[i].y + ctx->laserH < 0) {
            ctx->lasers[i].active = false;
            if (ctx->lasers[i].obj) lv_obj_add_flag(ctx->lasers[i].obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        if (ctx->lasers[i].obj && ctx->lasers[i].active) {
            lv_obj_set_pos(ctx->lasers[i].obj, (int)ctx->lasers[i].x, (int)ctx->lasers[i].y);
        }
    }

    // Check all laser-brick collisions once per tick
    checkLaserBrickCollisions(ctx);
}

static void checkLaserBrickCollisions(Context* ctx) {
    for (int li = 0; li < MAX_LASERS; li++) {
        if (!ctx->lasers[li].active) continue;

        for (int r = 0; r < ctx->rows; r++) {
            for (int c = 0; c < ctx->cols; c++) {
                int idx = r * ctx->cols + c;
                if (!ctx->brickAlive[idx]) continue;

                float bx = (float)(ctx->brickOffsetX + c * (ctx->brickW + ctx->brickGap));
                float by = (float)(ctx->brickOffsetY + r * (ctx->brickH + ctx->brickGap));

                if (ctx->lasers[li].x + ctx->laserW > bx && ctx->lasers[li].x < bx + ctx->brickW &&
                    ctx->lasers[li].y + ctx->laserH > by && ctx->lasers[li].y < by + ctx->brickH) {
                    hitBrick(ctx, idx);
                    ctx->lasers[li].active = false;
                    if (ctx->lasers[li].obj) lv_obj_add_flag(ctx->lasers[li].obj, LV_OBJ_FLAG_HIDDEN);
                    goto nextLaser;
                }
            }
        }
        nextLaser:;
    }
}

/* ── BreakOut Exit ────────────────────────────────────────────── */

static void openExit(Context* ctx) {
    ctx->exitOpen = true;
    if (ctx->exitIndicator) lv_obj_clear_flag(ctx->exitIndicator, LV_OBJ_FLAG_HIDDEN);
}

static void closeExit(Context* ctx) {
    ctx->exitOpen = false;
    if (ctx->exitIndicator) lv_obj_add_flag(ctx->exitIndicator, LV_OBJ_FLAG_HIDDEN);
}

/* ── Main Game Tick ───────────────────────────────────────────── */

static void update(Context* ctx) {
    if (ctx->state == GameState::Ready) {
        // Ball follows paddle
        ctx->balls[0].x = ctx->paddleX + ctx->paddleW / 2.0f - ctx->ballSize / 2.0f;
        if (ctx->balls[0].obj) lv_obj_set_x(ctx->balls[0].obj, (int)ctx->balls[0].x);
        return;
    }
    if (ctx->state != GameState::Playing) return;

    // Slow ball recovery
    if (ctx->slowRecoveryTicks > 0) {
        ctx->slowRecoveryTicks--;
        if (ctx->slowRecoveryTicks <= 0) {
            // Restore normal speed
            float targetSpeed = ctx->baseBallSpeed + (ctx->level - 1) * 0.3f;
            ctx->ballSpeed = targetSpeed;
        } else {
            // Gradually recover speed
            float targetSpeed = ctx->baseBallSpeed + (ctx->level - 1) * 0.3f;
            float progress = 1.0f - (float)ctx->slowRecoveryTicks / SLOW_RECOVERY_TICKS;
            ctx->ballSpeed = ctx->originalBallSpeed * 0.6f + (targetSpeed - ctx->originalBallSpeed * 0.6f) * progress;
        }
    }

    // Update all balls (movement, collisions)
    updateBalls(ctx);

    // Check if paddle reaches BreakOut exit
    if (ctx->exitOpen && ctx->paddleX + ctx->paddleW >= ctx->areaW - 8) {
        ctx->score += 10000;
        updateScoreDisplay(ctx);
        saveHighScore(ctx->score);
        if (ctx->sfxEngine) ctx->sfxEngine->play(SfxId::Warp);
        winLevel(ctx);
        return;
    }

    // Update capsules
    updateCapsules(ctx);

    // Update lasers
    updateLasers(ctx);
}

/* ── Display Updates ──────────────────────────────────────────── */

static void updateScoreDisplay(Context* ctx) {
    if (ctx->scoreLabel) {
        if (ctx->level > 1) {
            lv_label_set_text_fmt(ctx->scoreLabel, "L%d: %d", ctx->level, ctx->score);
        } else {
            lv_label_set_text_fmt(ctx->scoreLabel, "SCORE: %d", ctx->score);
        }
    }
    if (ctx->livesLabel) {
        lv_label_set_text_fmt(ctx->livesLabel, "L: %d", ctx->lives);
    }
}

static void updateMessage(Context* ctx) {
    if (!ctx->messageLabel) return;

    switch (ctx->state) {
        case GameState::Ready: {
            char buf[64];
            const char* input_hint = "Touch";
            if (device_has_active_by_type(&KEYBOARD_TYPE)) {
                input_hint = "Space";
            }
            if (ctx->level > 1) {
                snprintf(buf, sizeof(buf), "Level %d\n%s to start!", ctx->level, input_hint);
            } else if (highScore > 0) {
                snprintf(buf, sizeof(buf), "%s to start!\nBest Score: %d", input_hint, (int)highScore);
            } else {
                snprintf(buf, sizeof(buf), "%s to start!", input_hint);
            }
            lv_label_set_text(ctx->messageLabel, buf);
            lv_obj_clear_flag(ctx->messageLabel, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case GameState::Playing:
            lv_obj_add_flag(ctx->messageLabel, LV_OBJ_FLAG_HIDDEN);
            break;
        case GameState::Paused:
            lv_label_set_text(ctx->messageLabel, "PAUSED");
            lv_obj_clear_flag(ctx->messageLabel, LV_OBJ_FLAG_HIDDEN);
            break;
        case GameState::GameOver: {
            char buf[64];
            if (ctx->score > highScore && ctx->score > 0) {
                snprintf(buf, sizeof(buf), "NEW HIGH SCORE!\n%d", ctx->score);
            } else {
                snprintf(buf, sizeof(buf), "Game Over\nScore: %d\nBest Score: %d", ctx->score, (int)highScore);
            }
            lv_label_set_text(ctx->messageLabel, buf);
            lv_obj_clear_flag(ctx->messageLabel, LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }
    lv_obj_center(ctx->messageLabel);
}

static void updateSoundIcon(Context* ctx) {
    if (ctx->soundBtnIcon) {
        lv_label_set_text(ctx->soundBtnIcon, soundEnabled ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    }
}

/* ── Event Callbacks ──────────────────────────────────────────── */

static void onTick(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    if (ctx) update(ctx);
}

static void onPressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (!ctx || !ctx->paddle) return;
    if (ctx->state == GameState::GameOver || ctx->state == GameState::Paused) return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    // Move paddle to touch X (centered on finger)
    ctx->paddleX = (float)(point.x - ctx->paddleW / 2);

    // Clamp to game area bounds
    if (ctx->paddleX < 0) ctx->paddleX = 0;
    if (ctx->paddleX + ctx->paddleW > ctx->areaW)
        ctx->paddleX = (float)(ctx->areaW - ctx->paddleW);

    lv_obj_set_x(ctx->paddle, (int)ctx->paddleX);

    // In ready state, ball follows paddle
    if (ctx->state == GameState::Ready) {
        ctx->balls[0].x = ctx->paddleX + ctx->paddleW / 2.0f - ctx->ballSize / 2.0f;
        if (ctx->balls[0].obj) lv_obj_set_x(ctx->balls[0].obj, (int)ctx->balls[0].x);
    }
}

static void onClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (!ctx) return;

    if (ctx->state == GameState::Ready) {
        launchBall(ctx);
    } else if (ctx->state == GameState::GameOver) {
        startGame(ctx);
    } else if (ctx->state == GameState::Paused) {
        togglePause(ctx);
    } else if (ctx->state == GameState::Playing && ctx->catchActive && ctx->catchBallIndex >= 0) {
        launchBall(ctx);
    }
}

static void onKey(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (!ctx) return;

    uint32_t key = lv_event_get_key(e);

    switch (key) {
        case LV_KEY_LEFT:
        case 'a':
        case 'A':
        case ',':
            if (ctx->state == GameState::GameOver || ctx->state == GameState::Paused) break;
            ctx->paddleX -= ctx->paddleSpeed;
            if (ctx->paddleX < 0) ctx->paddleX = 0;
            if (ctx->paddle) lv_obj_set_x(ctx->paddle, (int)ctx->paddleX);
            if (ctx->state == GameState::Ready) resetBall(ctx);
            break;

        case LV_KEY_RIGHT:
        case 'd':
        case 'D':
        case '/':
            if (ctx->state == GameState::GameOver || ctx->state == GameState::Paused) break;
            ctx->paddleX += ctx->paddleSpeed;
            if (ctx->paddleX + ctx->paddleW > ctx->areaW)
                ctx->paddleX = (float)(ctx->areaW - ctx->paddleW);
            if (ctx->paddle) lv_obj_set_x(ctx->paddle, (int)ctx->paddleX);
            if (ctx->state == GameState::Ready) resetBall(ctx);
            break;

        case LV_KEY_ENTER:
        case ' ':
            if (ctx->state == GameState::Ready) {
                launchBall(ctx);
            } else if (ctx->state == GameState::GameOver) {
                startGame(ctx);
            } else if (ctx->state == GameState::Paused) {
                togglePause(ctx);
            } else if (ctx->state == GameState::Playing) {
                if (ctx->catchActive && ctx->catchBallIndex >= 0) {
                    launchBall(ctx);
                } else {
                    togglePause(ctx);
                }
            }
            break;
        case LV_KEY_ESC:
        case 'q':
        case 'Q': {
            // Exit key/edit mode - remove from group so navigation is restored
            // Re-entry: tap/click the game area to return to key mode
            lv_group_t* group = lv_group_get_default();
            if (group) lv_group_set_editing(group, false);
            lv_group_remove_obj(lv_event_get_current_target_obj(e));
            break;
        }
    }
}

static void onPauseClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx) togglePause(ctx);
}

static void onSoundToggled(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (!ctx) return;

    soundEnabled = !soundEnabled;
    if (ctx->sfxEngine) ctx->sfxEngine->setEnabled(soundEnabled);
    saveSoundSetting(soundEnabled);
    updateSoundIcon(ctx);
}

static void onReenterKeyMode(lv_event_t* e) {
    lv_obj_t* area = lv_event_get_current_target_obj(e);
    lv_group_t* group = lv_group_get_default();
    if (!group) return;
    if (lv_obj_get_group(area) == NULL) {
        lv_group_add_obj(group, area);
    }
    lv_group_focus_obj(area);
    lv_group_set_editing(group, true);
}
