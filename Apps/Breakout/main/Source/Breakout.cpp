/**
 * @file Breakout.cpp
 * @brief Breakout arcade game implementation for Tactility (Arkanoid-style)
 */
#include "Breakout.h"
#include "SfxEngine.h"

#include <cstdio>
#include <cmath>
#include <tt_lvgl_toolbar.h>
#include <tt_preferences.h>
#include <esp_random.h>
#include <tt_lvgl_keyboard.h>

#include <tactility/lvgl_module.h>

constexpr auto* TAG = "Breakout";

static constexpr const char* PREF_NAMESPACE = "Breakout";
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

static void loadSettings() {
    PreferencesHandle prefs = tt_preferences_alloc(PREF_NAMESPACE);
    if (prefs) {
        tt_preferences_opt_int32(prefs, PREF_HIGH_SCORE, &highScore);
        int32_t snd = 1;
        tt_preferences_opt_int32(prefs, PREF_SOUND, &snd);
        soundEnabled = (snd != 0);
        tt_preferences_free(prefs);
    }
}

static void saveHighScore(int32_t score) {
    if (score <= highScore) return;
    highScore = score;
    PreferencesHandle prefs = tt_preferences_alloc(PREF_NAMESPACE);
    if (prefs) {
        tt_preferences_put_int32(prefs, PREF_HIGH_SCORE, score);
        tt_preferences_free(prefs);
    }
}

static void saveSoundSetting(bool enabled) {
    soundEnabled = enabled;
    PreferencesHandle prefs = tt_preferences_alloc(PREF_NAMESPACE);
    if (prefs) {
        tt_preferences_put_int32(prefs, PREF_SOUND, enabled ? 1 : 0);
        tt_preferences_free(prefs);
    }
}

// Simple seeded random for procedural levels
static uint32_t levelRng(uint32_t& seed) {
    seed = seed * 1103515245 + 12345;
    return (seed >> 16) & 0x7FFF;
}

// ── UI Creation ──────────────────────────────────────────────

static int getToolbarHeight(UiDensity density) {
    if (density == LVGL_UI_DENSITY_COMPACT) {
        return 22;
    } else {
        return 40;
    }
}

void Breakout::onShow(AppHandle appHandle, lv_obj_t* parent) {
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 0, 0);
    lv_obj_set_style_pad_row(parent, 0, 0);

    // Load settings on first show
    if (needsInit) loadSettings();

    // Start sfx engine
    if (!sfxEngine) {
        sfxEngine = new SfxEngine();
        sfxEngine->start();
        sfxEngine->applyVolumePreset(SfxEngine::VolumePreset::Quiet);
        sfxEngine->setEnabled(soundEnabled);
    }

    // Toolbar
    lv_obj_t* toolbar = tt_lvgl_toolbar_create_for_app(parent, appHandle);

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

    scoreLabel = lv_label_create(scoreWrap);
    lv_obj_set_style_text_font(scoreLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(scoreLabel, lv_palette_main(LV_PALETTE_AMBER), 0);
    lv_obj_align(scoreLabel, LV_ALIGN_CENTER, 0, 0);

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

    livesLabel = lv_label_create(livesWrap);
    lv_obj_set_style_text_font(livesLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(livesLabel, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_align(livesLabel, LV_ALIGN_CENTER, 0, 0);

    // Toolbar buttons wrapper
    lv_obj_t* btnsWrapper = lv_obj_create(toolbar);
    lv_obj_set_width(btnsWrapper, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnsWrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btnsWrapper, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btnsWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btnsWrapper, 0, LV_STATE_DEFAULT);

    auto ui_density = lvgl_get_ui_density();
    auto toolbar_height = getToolbarHeight(ui_density);
    int btnSize = (ui_density == LVGL_UI_DENSITY_COMPACT) ? toolbar_height - 8 : toolbar_height - 6;

    // Pause button
    lv_obj_t* pauseBtn = lv_btn_create(btnsWrapper);
    lv_obj_set_size(pauseBtn, btnSize, btnSize);
    lv_obj_set_style_pad_all(pauseBtn, 0, LV_STATE_DEFAULT);
    lv_obj_align(pauseBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(pauseBtn, onPauseClicked, LV_EVENT_CLICKED, this);

    lv_obj_t* pauseIcon = lv_label_create(pauseBtn);
    lv_label_set_text(pauseIcon, LV_SYMBOL_PAUSE);
    lv_obj_align(pauseIcon, LV_ALIGN_CENTER, 0, 0);

    // Sound toggle button
    lv_obj_t* soundBtn = lv_btn_create(btnsWrapper);
    lv_obj_set_size(soundBtn, btnSize, btnSize);
    lv_obj_set_style_pad_all(soundBtn, 0, LV_STATE_DEFAULT);
    lv_obj_align(soundBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(soundBtn, onSoundToggled, LV_EVENT_CLICKED, this);

    soundBtnIcon = lv_label_create(soundBtn);
    lv_obj_align(soundBtnIcon, LV_ALIGN_CENTER, 0, 0);
    updateSoundIcon();

    // Screen size detection (The Book)
    lv_coord_t screenW = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenH = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenW < 280 || screenH < 180);
    bool isXLarge = (screenW >= 600);

    // Scaled dimensions
    cols = isSmall ? 8 : (isXLarge ? 12 : 10);
    rows = isSmall ? 3 : (isXLarge ? 5 : 4);
    brickW = isSmall ? 24 : (isXLarge ? 56 : 28);
    brickH = isSmall ? 8 : (isXLarge ? 18 : 10);
    brickGap = isSmall ? 2 : (isXLarge ? 4 : 2);
    ballSize = isSmall ? 6 : (isXLarge ? 14 : 8);
    paddleW = isSmall ? 40 : (isXLarge ? 100 : 54);
    paddleH = isSmall ? 6 : (isXLarge ? 14 : 8);
    baseBallSpeed = isSmall ? 2.0f : (isXLarge ? 4.0f : 2.5f);
    paddleSpeed = isSmall ? 16.0f : (isXLarge ? 36.0f : 24.0f);
    int paddleMargin = isSmall ? 2 : (isXLarge ? 8 : 4);
    int brickTopPad = isSmall ? 4 : (isXLarge ? 12 : 8);

    // Capsule dimensions
    capsuleW = isSmall ? 16 : (isXLarge ? 36 : 22);
    capsuleH = isSmall ? 8 : (isXLarge ? 16 : 12);
    capsuleFallSpeed = isSmall ? 1.2f : (isXLarge ? 2.5f : 1.8f);

    // Laser dimensions
    laserW = isSmall ? 2 : (isXLarge ? 4 : 3);
    laserH = isSmall ? 6 : (isXLarge ? 12 : 8);
    laserSpeed = isSmall ? 4.0f : (isXLarge ? 8.0f : 6.0f);

    // Store original paddle width for extend reset
    originalPaddleW = paddleW;

    // Ball speed includes level scaling
    ballSpeed = baseBallSpeed + (level - 1) * 0.3f;

    // Game area
    gameArea = lv_obj_create(parent);
    lv_obj_set_width(gameArea, LV_PCT(100));
    lv_obj_set_flex_grow(gameArea, 1);
    lv_obj_set_style_bg_color(gameArea, lv_color_hex(0x0a0a1e), 0);
    lv_obj_set_style_bg_opa(gameArea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(gameArea, 0, 0);
    lv_obj_set_style_pad_all(gameArea, 0, 0);
    lv_obj_set_style_radius(gameArea, 0, 0);
    lv_obj_remove_flag(gameArea, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(gameArea, LV_OBJ_FLAG_CLICKABLE);

    // Force layout to get accurate game area dimensions
    lv_obj_update_layout(parent);
    areaW = lv_obj_get_content_width(gameArea);
    areaH = lv_obj_get_content_height(gameArea);
    paddleYPos = areaH - paddleH - paddleMargin;

    // Calculate brick layout (centered horizontally)
    int totalBrickW = cols * brickW + (cols - 1) * brickGap;
    brickOffsetX = (areaW - totalBrickW) / 2;
    brickOffsetY = brickTopPad;

    // Create bricks
    createBricks();

    // Create paddle
    paddle = lv_obj_create(gameArea);
    lv_obj_set_size(paddle, paddleW, paddleH);
    lv_obj_set_style_bg_color(paddle, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
    lv_obj_set_style_border_width(paddle, 0, 0);
    lv_obj_set_style_pad_all(paddle, 0, 0);
    lv_obj_set_style_radius(paddle, 2, 0);
    lv_obj_remove_flag(paddle, LV_OBJ_FLAG_SCROLLABLE);

    // Create balls (primary + extras for split)
    for (int i = 0; i < MAX_BALLS; i++) {
        balls[i].obj = lv_obj_create(gameArea);
        lv_obj_set_size(balls[i].obj, ballSize, ballSize);
        lv_obj_set_style_bg_color(balls[i].obj, lv_color_white(), 0);
        lv_obj_set_style_border_width(balls[i].obj, 0, 0);
        lv_obj_set_style_pad_all(balls[i].obj, 0, 0);
        lv_obj_set_style_radius(balls[i].obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_remove_flag(balls[i].obj, LV_OBJ_FLAG_SCROLLABLE);
        if (i > 0) {
            lv_obj_add_flag(balls[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // Create capsule objects (pre-created, hidden)
    createCapsuleObjs();

    // Create laser objects (pre-created, hidden)
    createLaserObjs();

    // BreakOut exit indicator at paddle level (hidden by default)
    int exitH = paddleH * 3;
    exitIndicator = lv_obj_create(gameArea);
    lv_obj_set_size(exitIndicator, 6, exitH);
    lv_obj_set_style_bg_color(exitIndicator, lv_color_hex(0xFF44AA), 0);
    lv_obj_set_style_bg_opa(exitIndicator, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(exitIndicator, 0, 0);
    lv_obj_set_style_radius(exitIndicator, 0, 0);
    lv_obj_remove_flag(exitIndicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(exitIndicator, areaW - 6, paddleYPos - exitH / 2 + paddleH / 2);
    lv_obj_add_flag(exitIndicator, LV_OBJ_FLAG_HIDDEN);

    // Message overlay (centered in game area)
    messageLabel = lv_label_create(gameArea);
    lv_obj_set_style_text_color(messageLabel, lv_color_white(), 0);
    lv_obj_set_style_text_align(messageLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(messageLabel);

    // Initialize or restore
    if (needsInit) {
        paddleX = (areaW - paddleW) / 2.0f;
        startGame();
        needsInit = false;
    } else {
        // Restore visual positions from saved state
        lv_obj_set_pos(paddle, (int)paddleX, paddleYPos);
        for (int i = 0; i < MAX_BALLS; i++) {
            if (balls[i].active && balls[i].obj) {
                lv_obj_set_pos(balls[i].obj, (int)balls[i].x, (int)balls[i].y);
                lv_obj_clear_flag(balls[i].obj, LV_OBJ_FLAG_HIDDEN);
            }
        }
        updateScoreDisplay();
        updateMessage();
    }

    // Input handlers
    lv_obj_add_event_cb(gameArea, onPressed, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(gameArea, onClicked, LV_EVENT_SHORT_CLICKED, this);
    lv_obj_add_event_cb(gameArea, onKey, LV_EVENT_KEY, this);
    lv_obj_add_event_cb(gameArea, onFocused, LV_EVENT_FOCUSED, this);

    // Keyboard focus
    lv_group_t* group = lv_group_get_default();
    if (group) {
        lv_group_add_obj(group, gameArea);
        lv_group_focus_obj(gameArea);
        lv_group_set_editing(group, true);
    }

    // Start game timer
    gameTimer = lv_timer_create(onTick, TICK_MS, this);
}

void Breakout::onHide(AppHandle appHandle) {
    if (gameTimer) {
        lv_timer_delete(gameTimer);
        gameTimer = nullptr;
    }
    gameArea = nullptr;
    paddle = nullptr;
    for (int i = 0; i < MAX_BRICKS; i++) bricks[i] = nullptr;
    for (int i = 0; i < MAX_BALLS; i++) balls[i].obj = nullptr;
    for (int i = 0; i < MAX_CAPSULES; i++) {
        capsuleObjs[i] = nullptr;
        capsuleLabels[i] = nullptr;
    }
    for (int i = 0; i < MAX_LASERS; i++) lasers[i].obj = nullptr;
    exitIndicator = nullptr;
    scoreLabel = nullptr;
    livesLabel = nullptr;
    messageLabel = nullptr;
    soundBtnIcon = nullptr;

    // Clean up sfx engine
    if (sfxEngine) {
        sfxEngine->stop();
        delete sfxEngine;
        sfxEngine = nullptr;
    }
}

// ── Capsule & Laser Object Creation ─────────────────────────

void Breakout::createCapsuleObjs() {
    for (int i = 0; i < MAX_CAPSULES; i++) {
        capsuleObjs[i] = lv_obj_create(gameArea);
        lv_obj_set_size(capsuleObjs[i], capsuleW, capsuleH);
        lv_obj_set_style_border_width(capsuleObjs[i], 1, 0);
        lv_obj_set_style_border_color(capsuleObjs[i], lv_color_white(), 0);
        lv_obj_set_style_pad_all(capsuleObjs[i], 0, 0);
        lv_obj_set_style_radius(capsuleObjs[i], 3, 0);
        lv_obj_remove_flag(capsuleObjs[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(capsuleObjs[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);

        capsuleLabels[i] = lv_label_create(capsuleObjs[i]);
        lv_obj_set_style_text_color(capsuleLabels[i], lv_color_white(), 0);
        lv_obj_center(capsuleLabels[i]);
    }
}

void Breakout::createLaserObjs() {
    for (int i = 0; i < MAX_LASERS; i++) {
        lasers[i].obj = lv_obj_create(gameArea);
        lv_obj_set_size(lasers[i].obj, laserW, laserH);
        lv_obj_set_style_bg_color(lasers[i].obj, lv_color_hex(0xFF4444), 0);
        lv_obj_set_style_bg_opa(lasers[i].obj, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(lasers[i].obj, 0, 0);
        lv_obj_set_style_pad_all(lasers[i].obj, 0, 0);
        lv_obj_set_style_radius(lasers[i].obj, 0, 0);
        lv_obj_remove_flag(lasers[i].obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(lasers[i].obj, LV_OBJ_FLAG_HIDDEN);
        lasers[i].active = false;
    }
}

// ── Brick Creation ───────────────────────────────────────────

void Breakout::createBricks() {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (idx >= MAX_BRICKS) continue;
            bricks[idx] = lv_obj_create(gameArea);
            lv_obj_set_size(bricks[idx], brickW, brickH);
            lv_obj_set_style_border_width(bricks[idx], 0, 0);
            lv_obj_set_style_pad_all(bricks[idx], 0, 0);
            lv_obj_set_style_radius(bricks[idx], 2, 0);
            lv_obj_remove_flag(bricks[idx], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(bricks[idx], LV_OBJ_FLAG_CLICKABLE);

            int x = brickOffsetX + c * (brickW + brickGap);
            int y = brickOffsetY + r * (brickH + brickGap);
            lv_obj_set_pos(bricks[idx], x, y);
        }
    }
    refreshBricks();
}

void Breakout::setupLevelPattern() {
    int total = cols * rows;
    int numPatterns = 12;

    // Initialize all bricks
    for (int i = 0; i < total; i++) {
        brickAlive[i] = true;
        brickType[i] = BrickType::Normal;
        brickHits[i] = 1;
    }

    int colorOffset = (level - 1) % 8;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            brickColorIndex[r * cols + c] = (r + colorOffset) % 8;
        }
    }

    if (level <= numPatterns) {
        // Hardcoded patterns
        int pattern = (level - 1) % numPatterns;
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
        uint32_t seed = (uint32_t)level * 2654435761u;
        int density = 50 + (level * 2);
        if (density > 85) density = 85;

        for (int i = 0; i < total; i++) {
            int roll = (int)(levelRng(seed) % 100);
            brickAlive[i] = (roll < density);
        }
        // Ensure at least some bricks exist
        int aliveCount = 0;
        for (int i = 0; i < total; i++) if (brickAlive[i]) aliveCount++;
        if (aliveCount < 5) {
            for (int i = 0; i < total && aliveCount < 8; i++) {
                if (!brickAlive[i]) { brickAlive[i] = true; aliveCount++; }
            }
        }
    }

    // Add Silver bricks (level 3+)
    if (level >= 3) {
        int silverCount = (level - 2);
        if (silverCount > rows) silverCount = rows;
        int silverHits = (level >= 7) ? 3 : 2;
        uint32_t seed = (uint32_t)level * 31337u;
        int placed = 0;
        for (int attempt = 0; attempt < total * 2 && placed < silverCount; attempt++) {
            int idx = (int)(levelRng(seed) % total);
            if (brickAlive[idx] && brickType[idx] == BrickType::Normal) {
                brickType[idx] = BrickType::Silver;
                brickHits[idx] = silverHits;
                placed++;
            }
        }
    }

    // Add Gold bricks (level 5+)
    if (level >= 5) {
        int goldCount = (level - 4) / 2;
        if (goldCount > 4) goldCount = 4;
        uint32_t seed = (uint32_t)level * 48271u;
        int placed = 0;
        for (int attempt = 0; attempt < total * 2 && placed < goldCount; attempt++) {
            int idx = (int)(levelRng(seed) % total);
            if (brickAlive[idx] && brickType[idx] == BrickType::Normal) {
                brickType[idx] = BrickType::Gold;
                brickHits[idx] = INDESTRUCTIBLE_HITS; // indestructible
                placed++;
            }
        }
    }

    // Count alive bricks (excluding Gold)
    bricksRemaining = 0;
    destroyedCount = 0;
    for (int i = 0; i < total; i++) {
        if (brickAlive[i] && brickType[i] != BrickType::Gold) bricksRemaining++;
    }
}

void Breakout::refreshBricks() {
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int idx = r * cols + c;
            if (!bricks[idx]) continue;

            if (!brickAlive[idx]) {
                lv_obj_add_flag(bricks[idx], LV_OBJ_FLAG_HIDDEN);
                continue;
            }

            lv_obj_clear_flag(bricks[idx], LV_OBJ_FLAG_HIDDEN);

            switch (brickType[idx]) {
                case BrickType::Normal: {
                    lv_palette_t color = BRICK_COLORS[brickColorIndex[idx]];
                    lv_obj_set_style_bg_color(bricks[idx], lv_palette_main(color), 0);
                    lv_obj_set_style_border_width(bricks[idx], 0, 0);
                    break;
                }
                case BrickType::Silver: {
                    int darken = 3 - brickHits[idx]; // more hits taken = darker
                    if (darken < 0) darken = 0;
                    if (darken > 3) darken = 3;
                    lv_obj_set_style_bg_color(bricks[idx], lv_palette_darken(LV_PALETTE_GREY, darken), 0);
                    lv_obj_set_style_border_width(bricks[idx], 1, 0);
                    lv_obj_set_style_border_color(bricks[idx], lv_color_white(), 0);
                    break;
                }
                case BrickType::Gold:
                    lv_obj_set_style_bg_color(bricks[idx], lv_palette_main(LV_PALETTE_AMBER), 0);
                    lv_obj_set_style_border_width(bricks[idx], 1, 0);
                    lv_obj_set_style_border_color(bricks[idx], lv_palette_lighten(LV_PALETTE_AMBER, 2), 0);
                    break;
            }
        }
    }
}

// ── Brick Hit Logic ──────────────────────────────────────────

int Breakout::scoreBrick(int idx) {
    switch (brickType[idx]) {
        case BrickType::Silver:
            return 50 * level;
        case BrickType::Gold:
            return 0; // can't be destroyed
        case BrickType::Normal:
        default:
            return COLOR_SCORES[brickColorIndex[idx] % 8];
    }
}

void Breakout::hitBrick(int idx) {
    if (!brickAlive[idx]) return;

    if (brickType[idx] == BrickType::Gold) {
        // Bounce but don't damage
        if (sfxEngine) sfxEngine->play(SfxId::Click);
        return;
    }

    brickHits[idx]--;
    if (brickHits[idx] <= 0) {
        // Brick destroyed
        brickAlive[idx] = false;
        if (bricks[idx]) lv_obj_add_flag(bricks[idx], LV_OBJ_FLAG_HIDDEN);
        score += scoreBrick(idx);
        if (brickType[idx] != BrickType::Gold) {
            bricksRemaining--;
            destroyedCount++;
        }

        // Speed up ball slightly every 5 bricks destroyed
        if (destroyedCount % 5 == 0) {
            ballSpeed += 0.15f;
            // Scale active ball velocities
            for (int b = 0; b < MAX_BALLS; b++) {
                if (!balls[b].active) continue;
                float curSpd = std::sqrt(balls[b].vx * balls[b].vx + balls[b].vy * balls[b].vy);
                if (curSpd > 0.01f) {
                    float scale = ballSpeed / curSpd;
                    balls[b].vx *= scale;
                    balls[b].vy *= scale;
                }
            }
        }

        updateScoreDisplay();
        if (sfxEngine) sfxEngine->play(SfxId::BrickHit);

        // Try to spawn capsule (only when single ball)
        if (activeBallCount <= 1) {
            int bx = brickOffsetX + (idx % cols) * (brickW + brickGap);
            int by = brickOffsetY + (idx / cols) * (brickH + brickGap);
            if ((int)(esp_random() % 100) < CAPSULE_DROP_CHANCE) {
                spawnCapsule((float)bx, (float)by);
            }
        }

        if (bricksRemaining <= 0) {
            winLevel();
        }
    } else {
        // Multi-hit brick took damage (Silver)
        if (bricks[idx]) {
            int darken = 3 - brickHits[idx];
            if (darken < 0) darken = 0;
            if (darken > 3) darken = 3;
            lv_obj_set_style_bg_color(bricks[idx], lv_palette_darken(LV_PALETTE_GREY, darken), 0);
        }
        if (sfxEngine) sfxEngine->play(SfxId::Click);
    }
}

// ── Game State Management ────────────────────────────────────

void Breakout::startGame() {
    score = 0;
    lives = INITIAL_LIVES;
    level = 1;
    state = GameState::Ready;
    ballSpeed = baseBallSpeed;

    clearPowerUps();
    setupLevelPattern();
    refreshBricks();

    paddleX = (areaW - paddleW) / 2.0f;
    if (paddle) lv_obj_set_pos(paddle, (int)paddleX, paddleYPos);

    resetBall();
    updateScoreDisplay();
    updateMessage();

    if (sfxEngine) sfxEngine->play(SfxId::Confirm);
}

void Breakout::nextLevel() {
    level++;

    // Speed up ball each level
    ballSpeed = baseBallSpeed + (level - 1) * 0.3f;

    clearPowerUps();
    setupLevelPattern();
    refreshBricks();

    state = GameState::Ready;
    paddleX = (areaW - paddleW) / 2.0f;
    if (paddle) lv_obj_set_pos(paddle, (int)paddleX, paddleYPos);

    resetBall();
    updateScoreDisplay();
    updateMessage();

    if (sfxEngine) sfxEngine->play(SfxId::LevelUp);
}

void Breakout::resetBall() {
    // Reset to single ball
    for (int i = 1; i < MAX_BALLS; i++) {
        balls[i].active = false;
        if (balls[i].obj) lv_obj_add_flag(balls[i].obj, LV_OBJ_FLAG_HIDDEN);
    }
    activeBallCount = 1;

    balls[0].active = true;
    balls[0].x = paddleX + paddleW / 2.0f - ballSize / 2.0f;
    balls[0].y = (float)(paddleYPos - ballSize - 2);
    balls[0].vx = 0;
    balls[0].vy = 0;
    if (balls[0].obj) {
        lv_obj_set_pos(balls[0].obj, (int)balls[0].x, (int)balls[0].y);
        lv_obj_clear_flag(balls[0].obj, LV_OBJ_FLAG_HIDDEN);
    }
}

void Breakout::launchBall() {
    if (catchActive && catchBallIndex >= 0) {
        // Release caught ball
        BallState& b = balls[catchBallIndex];
        b.vx = (esp_random() % 2 ? 1.0f : -1.0f) * ballSpeed * 0.7f;
        b.vy = -ballSpeed;
        catchBallIndex = -1;
        catchAutoReleaseTicks = 0;
        if (sfxEngine) sfxEngine->play(SfxId::Confirm);
        return;
    }

    balls[0].vx = (esp_random() % 2 ? 1.0f : -1.0f) * ballSpeed * 0.7f;
    balls[0].vy = -ballSpeed;
    state = GameState::Playing;
    updateMessage();

    if (sfxEngine) sfxEngine->play(SfxId::Confirm);
}

void Breakout::loseLife() {
    lives--;
    clearPowerUps();

    if (sfxEngine) sfxEngine->play(SfxId::Hurt);

    if (lives <= 0) {
        state = GameState::GameOver;
        for (int i = 0; i < MAX_BALLS; i++) {
            balls[i].vx = 0;
            balls[i].vy = 0;
        }
        if (sfxEngine) sfxEngine->play(SfxId::GameOver);
    } else {
        state = GameState::Ready;
        paddleX = (areaW - paddleW) / 2.0f;
        if (paddle) lv_obj_set_pos(paddle, (int)paddleX, paddleYPos);
        resetBall();
    }
    updateScoreDisplay();
    updateMessage();
    if (lives <= 0) {
        saveHighScore(score);
    }
}

void Breakout::winLevel() {
    saveHighScore(score);
    nextLevel();
}

void Breakout::togglePause() {
    if (state == GameState::Playing) {
        state = GameState::Paused;
        updateMessage();
    } else if (state == GameState::Paused) {
        state = GameState::Playing;
        updateMessage();
    }
}

// ── Power-Up System ──────────────────────────────────────────

void Breakout::spawnCapsule(float x, float y) {
    for (int i = 0; i < MAX_CAPSULES; i++) {
        if (!capsules[i].active) {
            capsules[i].active = true;
            capsules[i].x = x;
            capsules[i].y = y;

            // Random power-up type (ExtraLife rarer)
            int roll = (int)(esp_random() % 100);
            if (roll < 5) {
                capsules[i].type = PowerUpType::ExtraLife;
            } else if (roll < 18) {
                capsules[i].type = PowerUpType::Laser;
            } else if (roll < 33) {
                capsules[i].type = PowerUpType::Extend;
            } else if (roll < 48) {
                capsules[i].type = PowerUpType::Catch;
            } else if (roll < 63) {
                capsules[i].type = PowerUpType::Slow;
            } else if (roll < 78) {
                capsules[i].type = PowerUpType::Split;
            } else {
                capsules[i].type = PowerUpType::BreakOut;
            }

            int typeIdx = static_cast<int>(capsules[i].type);
            if (capsuleObjs[i]) {
                lv_obj_set_style_bg_color(capsuleObjs[i], lv_color_hex(CAPSULE_COLORS[typeIdx]), 0);
                lv_obj_set_pos(capsuleObjs[i], (int)x, (int)y);
                lv_obj_clear_flag(capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
            }
            if (capsuleLabels[i]) {
                lv_label_set_text(capsuleLabels[i], CAPSULE_LETTERS[typeIdx]);
            }
            return;
        }
    }
}

void Breakout::updateCapsules() {
    for (int i = 0; i < MAX_CAPSULES; i++) {
        if (!capsules[i].active) continue;

        capsules[i].y += capsuleFallSpeed;

        // Off screen
        if (capsules[i].y > areaH) {
            capsules[i].active = false;
            if (capsuleObjs[i]) lv_obj_add_flag(capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        // Paddle collision
        if (capsules[i].y + capsuleH > paddleYPos &&
            capsules[i].y < paddleYPos + paddleH &&
            capsules[i].x + capsuleW > paddleX &&
            capsules[i].x < paddleX + paddleW) {
            activatePowerUp(capsules[i].type);
            capsules[i].active = false;
            if (capsuleObjs[i]) lv_obj_add_flag(capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        if (capsuleObjs[i]) lv_obj_set_pos(capsuleObjs[i], (int)capsules[i].x, (int)capsules[i].y);
    }
}

void Breakout::activatePowerUp(PowerUpType type) {
    if (sfxEngine) sfxEngine->play(SfxId::Powerup);

    switch (type) {
        case PowerUpType::Extend:
            if (!extendActive) {
                extendActive = true;
                paddleW = (int)(originalPaddleW * 1.5f);
                int maxW = areaW / 2;
                if (paddleW > maxW) paddleW = maxW;
                if (paddle) lv_obj_set_width(paddle, paddleW);
                // Re-clamp paddle position
                if (paddleX + paddleW > areaW) paddleX = (float)(areaW - paddleW);
                if (paddle) lv_obj_set_x(paddle, (int)paddleX);
            }
            break;

        case PowerUpType::ExtraLife:
            lives++;
            if (sfxEngine) sfxEngine->play(SfxId::OneUp);
            updateScoreDisplay();
            break;

        case PowerUpType::Slow:
            if (slowRecoveryTicks <= 0) {
                // First slow: save speed and apply reduction
                originalBallSpeed = ballSpeed;
                ballSpeed *= 0.6f;
                // Scale all active ball velocities
                for (int b = 0; b < MAX_BALLS; b++) {
                    if (!balls[b].active) continue;
                    float curSpd = std::sqrt(balls[b].vx * balls[b].vx + balls[b].vy * balls[b].vy);
                    if (curSpd > 0.01f) {
                        float scale = ballSpeed / curSpd;
                        balls[b].vx *= scale;
                        balls[b].vy *= scale;
                    }
                }
            }
            // Reset (or extend) recovery timer
            slowRecoveryTicks = SLOW_RECOVERY_TICKS;
            break;

        case PowerUpType::Catch:
            catchActive = true;
            catchBallIndex = -1;
            break;

        case PowerUpType::Split:
            splitBalls();
            break;

        case PowerUpType::Laser:
            laserActive = true;
            laserCooldown = 0;
            break;

        case PowerUpType::BreakOut:
            openExit();
            break;
    }
}

void Breakout::clearPowerUps() {
    // Reset extend
    if (extendActive) {
        extendActive = false;
        paddleW = originalPaddleW;
        if (paddle) lv_obj_set_width(paddle, paddleW);
    }

    // Reset catch
    catchActive = false;
    catchBallIndex = -1;
    catchAutoReleaseTicks = 0;

    // Reset slow
    if (slowRecoveryTicks > 0) {
        ballSpeed = baseBallSpeed + (level - 1) * 0.3f;
        slowRecoveryTicks = 0;
    }

    // Reset laser
    laserActive = false;
    laserCooldown = 0;
    for (int i = 0; i < MAX_LASERS; i++) {
        lasers[i].active = false;
        if (lasers[i].obj) lv_obj_add_flag(lasers[i].obj, LV_OBJ_FLAG_HIDDEN);
    }

    // Clear capsules
    for (int i = 0; i < MAX_CAPSULES; i++) {
        capsules[i].active = false;
        if (capsuleObjs[i]) lv_obj_add_flag(capsuleObjs[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Close exit
    closeExit();
}

// ── Multi-Ball ───────────────────────────────────────────────

void Breakout::splitBalls() {
    // Find first active ball to split from
    int sourceIdx = -1;
    for (int i = 0; i < MAX_BALLS; i++) {
        if (balls[i].active) { sourceIdx = i; break; }
    }
    if (sourceIdx < 0) return;

    BallState& src = balls[sourceIdx];
    int spawned = 0;
    for (int i = 0; i < MAX_BALLS && spawned < 2; i++) {
        if (balls[i].active) continue;
        balls[i].active = true;
        balls[i].x = src.x;
        balls[i].y = src.y;

        // Diverging angles: +30 and -30 degrees from source
        float angle = (spawned == 0) ? 0.5f : -0.5f;
        float speed = std::sqrt(src.vx * src.vx + src.vy * src.vy);
        if (speed < 0.01f) speed = ballSpeed;
        float srcAngle = std::atan2(src.vy, src.vx);
        balls[i].vx = speed * std::cos(srcAngle + angle);
        balls[i].vy = speed * std::sin(srcAngle + angle);

        if (balls[i].obj) {
            lv_obj_set_pos(balls[i].obj, (int)balls[i].x, (int)balls[i].y);
            lv_obj_clear_flag(balls[i].obj, LV_OBJ_FLAG_HIDDEN);
        }
        spawned++;
    }
    activeBallCount += spawned;
}

void Breakout::updateBalls() {
    for (int b = 0; b < MAX_BALLS; b++) {
        if (!balls[b].active) continue;

        // Caught ball follows paddle
        if (catchActive && catchBallIndex == b) {
            balls[b].x = paddleX + catchOffsetX;
            balls[b].y = (float)(paddleYPos - ballSize - 2);
            if (balls[b].obj) lv_obj_set_pos(balls[b].obj, (int)balls[b].x, (int)balls[b].y);

            catchAutoReleaseTicks++;
            if (catchAutoReleaseTicks >= CATCH_AUTO_RELEASE_TICKS) {
                // Auto-release
                balls[b].vx = (esp_random() % 2 ? 1.0f : -1.0f) * ballSpeed * 0.7f;
                balls[b].vy = -ballSpeed;
                catchBallIndex = -1;
                catchAutoReleaseTicks = 0;
            }
            continue;
        }

        // Move ball
        balls[b].x += balls[b].vx;
        balls[b].y += balls[b].vy;

        // Left wall collision
        if (balls[b].x < 0) {
            balls[b].x = 0;
            balls[b].vx = -balls[b].vx;
            if (sfxEngine) sfxEngine->play(SfxId::Click);
        }

        // Right wall collision (ball always bounces, exit is paddle-only)
        if (balls[b].x + ballSize > areaW) {
            balls[b].x = (float)(areaW - ballSize);
            balls[b].vx = -balls[b].vx;
            if (sfxEngine) sfxEngine->play(SfxId::Click);
        }

        // Top wall
        if (balls[b].y < 0) {
            balls[b].y = 0;
            balls[b].vy = -balls[b].vy;
            if (sfxEngine) sfxEngine->play(SfxId::Click);
        }

        // Bottom edge
        if (balls[b].y + ballSize > areaH) {
            balls[b].active = false;
            if (balls[b].obj) lv_obj_add_flag(balls[b].obj, LV_OBJ_FLAG_HIDDEN);
            activeBallCount--;
            if (activeBallCount <= 0) {
                activeBallCount = 0;
                loseLife();
                return;
            }
            continue;
        }

        // Paddle collision
        if (balls[b].vy > 0 &&
            balls[b].x + ballSize > paddleX && balls[b].x < paddleX + paddleW &&
            balls[b].y + ballSize > paddleYPos && balls[b].y < paddleYPos + paddleH) {

            if (catchActive && catchBallIndex < 0) {
                // Catch the ball
                catchBallIndex = b;
                catchOffsetX = balls[b].x - paddleX;
                catchAutoReleaseTicks = 0;
                balls[b].vx = 0;
                balls[b].vy = 0;
                balls[b].y = (float)(paddleYPos - ballSize - 2);
                if (sfxEngine) sfxEngine->play(SfxId::Click);
            } else {
                // Normal bounce
                float hitPos = (balls[b].x + ballSize / 2.0f - paddleX) / (float)paddleW;
                float angle = (hitPos - 0.5f) * 2.0f;
                balls[b].vx = angle * ballSpeed;
                balls[b].vy = -std::fabs(balls[b].vy);
                if (std::fabs(balls[b].vy) < ballSpeed * 0.3f) {
                    balls[b].vy = -ballSpeed * 0.3f;
                }
                balls[b].y = (float)(paddleYPos - ballSize);
                if (sfxEngine) sfxEngine->play(SfxId::Click);
            }
        }

        // Brick collisions for this ball
        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                if (!brickAlive[idx]) continue;

                float bx = (float)(brickOffsetX + c * (brickW + brickGap));
                float by = (float)(brickOffsetY + r * (brickH + brickGap));

                if (balls[b].x + ballSize > bx && balls[b].x < bx + brickW &&
                    balls[b].y + ballSize > by && balls[b].y < by + brickH) {

                    hitBrick(idx);

                    // Bounce direction
                    float overlapLeft = balls[b].x + ballSize - bx;
                    float overlapRight = bx + brickW - balls[b].x;
                    float overlapTop = balls[b].y + ballSize - by;
                    float overlapBottom = by + brickH - balls[b].y;
                    float minOverlapX = overlapLeft < overlapRight ? overlapLeft : overlapRight;
                    float minOverlapY = overlapTop < overlapBottom ? overlapTop : overlapBottom;

                    if (minOverlapX < minOverlapY) {
                        balls[b].vx = -balls[b].vx;
                    } else {
                        balls[b].vy = -balls[b].vy;
                    }

                    goto nextBall; // One brick per ball per tick
                }
            }
        }

        nextBall:
        if (balls[b].obj && balls[b].active) {
            lv_obj_set_pos(balls[b].obj, (int)balls[b].x, (int)balls[b].y);
        }
    }
}

// ── Laser System ─────────────────────────────────────────────

void Breakout::fireLaser() {
    for (int i = 0; i < MAX_LASERS; i++) {
        if (!lasers[i].active) {
            lasers[i].active = true;
            lasers[i].x = paddleX + paddleW / 2.0f - laserW / 2.0f;
            lasers[i].y = (float)(paddleYPos - laserH);
            if (lasers[i].obj) {
                lv_obj_set_pos(lasers[i].obj, (int)lasers[i].x, (int)lasers[i].y);
                lv_obj_clear_flag(lasers[i].obj, LV_OBJ_FLAG_HIDDEN);
            }
            if (sfxEngine) sfxEngine->play(SfxId::Laser);
            return;
        }
    }
}

void Breakout::updateLasers() {
    if (!laserActive) return;

    // Auto-fire
    laserCooldown--;
    if (laserCooldown <= 0) {
        fireLaser();
        laserCooldown = LASER_COOLDOWN_TICKS;
    }

    for (int i = 0; i < MAX_LASERS; i++) {
        if (!lasers[i].active) continue;

        lasers[i].y -= laserSpeed;

        if (lasers[i].y + laserH < 0) {
            lasers[i].active = false;
            if (lasers[i].obj) lv_obj_add_flag(lasers[i].obj, LV_OBJ_FLAG_HIDDEN);
            continue;
        }

        if (lasers[i].obj && lasers[i].active) {
            lv_obj_set_pos(lasers[i].obj, (int)lasers[i].x, (int)lasers[i].y);
        }
    }

    // Check all laser-brick collisions once per tick
    checkLaserBrickCollisions();
}

void Breakout::checkLaserBrickCollisions() {
    for (int li = 0; li < MAX_LASERS; li++) {
        if (!lasers[li].active) continue;

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                if (!brickAlive[idx]) continue;

                float bx = (float)(brickOffsetX + c * (brickW + brickGap));
                float by = (float)(brickOffsetY + r * (brickH + brickGap));

                if (lasers[li].x + laserW > bx && lasers[li].x < bx + brickW &&
                    lasers[li].y + laserH > by && lasers[li].y < by + brickH) {
                    hitBrick(idx);
                    lasers[li].active = false;
                    if (lasers[li].obj) lv_obj_add_flag(lasers[li].obj, LV_OBJ_FLAG_HIDDEN);
                    goto nextLaser;
                }
            }
        }
        nextLaser:;
    }
}

// ── BreakOut Exit ────────────────────────────────────────────

void Breakout::openExit() {
    exitOpen = true;
    if (exitIndicator) lv_obj_clear_flag(exitIndicator, LV_OBJ_FLAG_HIDDEN);
}

void Breakout::closeExit() {
    exitOpen = false;
    if (exitIndicator) lv_obj_add_flag(exitIndicator, LV_OBJ_FLAG_HIDDEN);
}

// ── Main Game Tick ───────────────────────────────────────────

void Breakout::update() {
    if (state == GameState::Ready) {
        // Ball follows paddle
        balls[0].x = paddleX + paddleW / 2.0f - ballSize / 2.0f;
        if (balls[0].obj) lv_obj_set_x(balls[0].obj, (int)balls[0].x);
        return;
    }
    if (state != GameState::Playing) return;

    // Slow ball recovery
    if (slowRecoveryTicks > 0) {
        slowRecoveryTicks--;
        if (slowRecoveryTicks <= 0) {
            // Restore normal speed
            float targetSpeed = baseBallSpeed + (level - 1) * 0.3f;
            ballSpeed = targetSpeed;
        } else {
            // Gradually recover speed
            float targetSpeed = baseBallSpeed + (level - 1) * 0.3f;
            float progress = 1.0f - (float)slowRecoveryTicks / SLOW_RECOVERY_TICKS;
            ballSpeed = originalBallSpeed * 0.6f + (targetSpeed - originalBallSpeed * 0.6f) * progress;
        }
    }

    // Update all balls (movement, collisions)
    updateBalls();

    // Check if paddle reaches BreakOut exit
    if (exitOpen && paddleX + paddleW >= areaW - 8) {
        score += 10000;
        updateScoreDisplay();
        saveHighScore(score);
        if (sfxEngine) sfxEngine->play(SfxId::Warp);
        winLevel();
        return;
    }

    // Update capsules
    updateCapsules();

    // Update lasers
    updateLasers();
}

// ── Display Updates ──────────────────────────────────────────

void Breakout::updateScoreDisplay() {
    if (scoreLabel) {
        if (level > 1) {
            lv_label_set_text_fmt(scoreLabel, "L%d: %d", level, score);
        } else {
            lv_label_set_text_fmt(scoreLabel, "SCORE: %d", score);
        }
    }
    if (livesLabel) {
        lv_label_set_text_fmt(livesLabel, "L: %d", lives);
    }
}

void Breakout::updateMessage() {
    if (!messageLabel) return;

    switch (state) {
        case GameState::Ready: {
            char buf[64];
            const char* input_hint = "Touch";
            if (tt_lvgl_hardware_keyboard_is_available()) {
                input_hint = "Space";
            }
            if (level > 1) {
                snprintf(buf, sizeof(buf), "Level %d\n%s to start!", level, input_hint);
            } else if (highScore > 0) {
                snprintf(buf, sizeof(buf), "%s to start!\nBest Score: %d", input_hint, (int)highScore);
            } else {
                snprintf(buf, sizeof(buf), "%s to start!", input_hint);
            }
            lv_label_set_text(messageLabel, buf);
            lv_obj_clear_flag(messageLabel, LV_OBJ_FLAG_HIDDEN);
            break;
        }
        case GameState::Playing:
            lv_obj_add_flag(messageLabel, LV_OBJ_FLAG_HIDDEN);
            break;
        case GameState::Paused:
            lv_label_set_text(messageLabel, "PAUSED");
            lv_obj_clear_flag(messageLabel, LV_OBJ_FLAG_HIDDEN);
            break;
        case GameState::GameOver: {
            char buf[64];
            if (score > highScore && score > 0) {
                snprintf(buf, sizeof(buf), "NEW HIGH SCORE!\n%d", score);
            } else {
                snprintf(buf, sizeof(buf), "Game Over\nScore: %d\nBest Score: %d", score, (int)highScore);
            }
            lv_label_set_text(messageLabel, buf);
            lv_obj_clear_flag(messageLabel, LV_OBJ_FLAG_HIDDEN);
            break;
        }
    }
    lv_obj_center(messageLabel);
}

void Breakout::updateSoundIcon() {
    if (soundBtnIcon) {
        lv_label_set_text(soundBtnIcon, soundEnabled ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_MUTE);
    }
}

// ── Event Callbacks ──────────────────────────────────────────

void Breakout::onTick(lv_timer_t* timer) {
    Breakout* self = static_cast<Breakout*>(lv_timer_get_user_data(timer));
    if (self) self->update();
}

void Breakout::onPressed(lv_event_t* e) {
    Breakout* self = static_cast<Breakout*>(lv_event_get_user_data(e));
    if (!self || !self->paddle) return;
    if (self->state == GameState::GameOver || self->state == GameState::Paused) return;

    lv_point_t point;
    lv_indev_get_point(lv_indev_active(), &point);

    // Move paddle to touch X (centered on finger)
    self->paddleX = (float)(point.x - self->paddleW / 2);

    // Clamp to game area bounds
    if (self->paddleX < 0) self->paddleX = 0;
    if (self->paddleX + self->paddleW > self->areaW)
        self->paddleX = (float)(self->areaW - self->paddleW);

    lv_obj_set_x(self->paddle, (int)self->paddleX);

    // In ready state, ball follows paddle
    if (self->state == GameState::Ready) {
        self->balls[0].x = self->paddleX + self->paddleW / 2.0f - self->ballSize / 2.0f;
        if (self->balls[0].obj) lv_obj_set_x(self->balls[0].obj, (int)self->balls[0].x);
    }
}

void Breakout::onClicked(lv_event_t* e) {
    Breakout* self = static_cast<Breakout*>(lv_event_get_user_data(e));
    if (!self) return;

    if (self->state == GameState::Ready) {
        self->launchBall();
    } else if (self->state == GameState::GameOver) {
        self->startGame();
    } else if (self->state == GameState::Paused) {
        self->togglePause();
    } else if (self->state == GameState::Playing && self->catchActive && self->catchBallIndex >= 0) {
        self->launchBall();
    }
}

void Breakout::onKey(lv_event_t* e) {
    Breakout* self = static_cast<Breakout*>(lv_event_get_user_data(e));
    if (!self) return;

    uint32_t key = lv_event_get_key(e);

    switch (key) {
        case LV_KEY_LEFT:
        case 'a':
        case 'A':
        case ',':
            if (self->state == GameState::GameOver || self->state == GameState::Paused) break;
            self->paddleX -= self->paddleSpeed;
            if (self->paddleX < 0) self->paddleX = 0;
            if (self->paddle) lv_obj_set_x(self->paddle, (int)self->paddleX);
            if (self->state == GameState::Ready) self->resetBall();
            break;

        case LV_KEY_RIGHT:
        case 'd':
        case 'D':
        case '/':
            if (self->state == GameState::GameOver || self->state == GameState::Paused) break;
            self->paddleX += self->paddleSpeed;
            if (self->paddleX + self->paddleW > self->areaW)
                self->paddleX = (float)(self->areaW - self->paddleW);
            if (self->paddle) lv_obj_set_x(self->paddle, (int)self->paddleX);
            if (self->state == GameState::Ready) self->resetBall();
            break;

        case LV_KEY_ENTER:
        case ' ':
            if (self->state == GameState::Ready) {
                self->launchBall();
            } else if (self->state == GameState::GameOver) {
                self->startGame();
            } else if (self->state == GameState::Paused) {
                self->togglePause();
            } else if (self->state == GameState::Playing) {
                if (self->catchActive && self->catchBallIndex >= 0) {
                    self->launchBall();
                } else {
                    self->togglePause();
                }
            }
            break;
    }
}

void Breakout::onPauseClicked(lv_event_t* e) {
    Breakout* self = static_cast<Breakout*>(lv_event_get_user_data(e));
    if (self) self->togglePause();
}

void Breakout::onSoundToggled(lv_event_t* e) {
    Breakout* self = static_cast<Breakout*>(lv_event_get_user_data(e));
    if (!self) return;

    soundEnabled = !soundEnabled;
    if (self->sfxEngine) self->sfxEngine->setEnabled(soundEnabled);
    saveSoundSetting(soundEnabled);
    self->updateSoundIcon();
}

void Breakout::onFocused(lv_event_t* e) {
    lv_group_t* group = lv_group_get_default();
    if (group) lv_group_set_editing(group, true);
}
