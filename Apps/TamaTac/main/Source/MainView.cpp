/**
 * @file MainView.cpp
 * @brief Main pet view implementation
 */

#include "MainView.h"
#include "TamaTac.h"
#include <Tactility/kernel/Kernel.h>
#include <cstdio>

extern lv_color_t TamaTac_canvasBuffer[72 * 72];
extern lv_color_t TamaTac_iconBuffers[12][16 * 16];

void MainView::onStart(lv_obj_t* parentWidget, TamaTac* appInstance) {
    parent = parentWidget;
    app = appInstance;

    // Detect screen size for responsive layout
    lv_coord_t screenWidth = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t screenHeight = lv_display_get_vertical_resolution(nullptr);
    bool isSmall = (screenWidth < 280 || screenHeight < 180);
    bool isXLarge = (screenWidth >= 600);
    bool isLarge = !isXLarge && (screenWidth >= 400 && screenHeight >= 300);

    // Scaled dimensions based on screen tier
    int petAreaSize, statBarWidth, statBarHeight;
    int btnWidth, btnHeight, padMain, padRow, poopHeight;

    if (isSmall) {
        petAreaSize = 56; petCanvasSize = 48; spriteScale = 2;  // 24*2=48
        statBarWidth = 85; statBarHeight = 5;
        btnWidth = 54; btnHeight = 32;
        padMain = 1; padRow = 1; poopHeight = 14;
    } else if (isXLarge) {
        petAreaSize = 136; petCanvasSize = 72; spriteScale = 3;  // 24*3=72
        statBarWidth = 300; statBarHeight = 10;
        btnWidth = 120; btnHeight = 56;
        padMain = 8; padRow = 6; poopHeight = 24;
    } else if (isLarge) {
        petAreaSize = 100; petCanvasSize = 72; spriteScale = 3;
        statBarWidth = 200; statBarHeight = 8;
        btnWidth = 90; btnHeight = 50;
        padMain = 4; padRow = 4; poopHeight = 22;
    } else {
        // Medium (default)
        petAreaSize = 76; petCanvasSize = 72; spriteScale = 3;
        statBarWidth = 135; statBarHeight = 6;
        btnWidth = 70; btnHeight = 42;
        padMain = 1; padRow = 1; poopHeight = 16;
    }

    // Initialize animation state
    animStartTime = tt::kernel::getMillis();
    currentSpriteId = SPRITE_EGG_IDLE;

    // Main content wrapper
    mainWrapper = lv_obj_create(parent);
    lv_obj_set_size(mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(mainWrapper, padMain, 0);
    lv_obj_set_style_bg_opa(mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mainWrapper, 0, 0);
    lv_obj_set_flex_flow(mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Pet and stats container (horizontal layout)
    lv_obj_t* petStatsRow = lv_obj_create(mainWrapper);
    lv_obj_set_size(petStatsRow, LV_PCT(98), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(petStatsRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(petStatsRow, 0, 0);
    lv_obj_set_style_pad_all(petStatsRow, 0, 0);
    lv_obj_set_style_pad_left(petStatsRow, isSmall ? 2 : (isXLarge ? 16 : 8), 0);
    lv_obj_set_flex_flow(petStatsRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(petStatsRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    // Pet column (left side)
    lv_obj_t* petColumn = lv_obj_create(petStatsRow);
    lv_obj_set_size(petColumn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(petColumn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(petColumn, 0, 0);
    lv_obj_set_style_pad_all(petColumn, 0, 0);
    lv_obj_set_style_pad_row(petColumn, isSmall ? 1 : (isXLarge ? 8 : 2), 0);
    lv_obj_set_flex_flow(petColumn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(petColumn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Pet display area
    petArea = lv_obj_create(petColumn);
    lv_obj_set_size(petArea, petAreaSize, petAreaSize);
    lv_obj_set_style_bg_color(petArea, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(petArea, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(petArea, isSmall ? 1 : (isXLarge ? 3 : 2), 0);
    lv_obj_set_style_pad_all(petArea, 0, 0);

    // Make pet area tappable for pet interaction
    lv_obj_add_flag(petArea, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(petArea, onPetTapped, LV_EVENT_CLICKED, app);

    // Create canvas for sprite rendering
    petCanvas = lv_canvas_create(petArea);
    lv_canvas_set_buffer(petCanvas, TamaTac_canvasBuffer, petCanvasSize, petCanvasSize, LV_COLOR_FORMAT_NATIVE);
    lv_obj_center(petCanvas);

    // Poop container
    poopContainer = lv_obj_create(petColumn);
    lv_obj_set_size(poopContainer, petAreaSize, poopHeight);
    lv_obj_set_style_bg_opa(poopContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(poopContainer, 0, 0);
    lv_obj_set_style_pad_all(poopContainer, 1, 0);
    lv_obj_set_style_pad_column(poopContainer, isSmall ? 1 : (isXLarge ? 4 : 2), 0);
    lv_obj_set_flex_flow(poopContainer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(poopContainer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(poopContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Create 3 poop icon canvases
    for (int i = 0; i < 3; i++) {
        poopIcons[i] = lv_canvas_create(poopContainer);
        lv_canvas_set_buffer(poopIcons[i], TamaTac_iconBuffers[i], 16, 16, LV_COLOR_FORMAT_NATIVE);
        lv_obj_add_flag(poopIcons[i], LV_OBJ_FLAG_HIDDEN);
    }

    // Stat bars container
    lv_obj_t* statsContainer = lv_obj_create(petStatsRow);
    lv_obj_set_size(statsContainer, LV_SIZE_CONTENT, petAreaSize);
    lv_obj_set_style_bg_opa(statsContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(statsContainer, 0, 0);
    lv_obj_set_style_pad_all(statsContainer, padRow, 0);
    lv_obj_set_style_pad_left(statsContainer, isSmall ? 4 : (isXLarge ? 16 : 8), 0);
    lv_obj_set_style_pad_row(statsContainer, padRow, 0);
    lv_obj_set_flex_flow(statsContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(statsContainer, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_grow(statsContainer, 1);
    lv_obj_remove_flag(statsContainer, LV_OBJ_FLAG_SCROLLABLE);

    // Helper to create stat row
    auto createStatRow = [&](lv_obj_t* parent, int iconBufferIdx, IconId iconId,
                             lv_obj_t** bar, lv_color_t barColor) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, isSmall ? 2 : (isXLarge ? 6 : 3), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* iconCanvas = lv_canvas_create(row);
        lv_canvas_set_buffer(iconCanvas, TamaTac_iconBuffers[iconBufferIdx], 16, 16, LV_COLOR_FORMAT_NATIVE);
        drawIcon(iconCanvas, iconId);

        *bar = lv_bar_create(row);
        lv_obj_set_size(*bar, statBarWidth, statBarHeight);
        lv_bar_set_range(*bar, 0, 100);
        lv_obj_set_style_bg_color(*bar, lv_color_hex(0x444444), LV_PART_MAIN);
        lv_obj_set_style_bg_color(*bar, barColor, LV_PART_INDICATOR);

        return iconCanvas;
    };

    statIcons[0] = createStatRow(statsContainer, 3, ICON_HUNGER, &hungerBar, lv_color_hex(0xFF9900));
    statIcons[1] = createStatRow(statsContainer, 4, ICON_HAPPINESS, &happinessBar, lv_color_hex(0xFFCC00));
    statIcons[2] = createStatRow(statsContainer, 5, ICON_HEALTH, &healthBar, lv_color_hex(0x00FF00));
    statIcons[3] = createStatRow(statsContainer, 6, ICON_ENERGY, &energyBar, lv_color_hex(0x00CCFF));

    // Status label
    statusLabel = lv_label_create(mainWrapper);
    lv_label_set_text(statusLabel, "Your pet is ready!");
    lv_obj_set_style_text_align(statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(statusLabel, 0, 0);
    lv_obj_set_style_pad_bottom(statusLabel, 0, 0);

    // Button container
    lv_obj_t* btnContainer = lv_obj_create(mainWrapper);
    lv_obj_set_width(btnContainer, LV_PCT(100));
    lv_obj_set_height(btnContainer, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btnContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnContainer, 0, 0);
    lv_obj_set_style_pad_all(btnContainer, 0, 0);
    lv_obj_set_style_pad_column(btnContainer, isSmall ? 3 : (isXLarge ? 16 : 6), 0);
    lv_obj_set_style_pad_top(btnContainer, padRow, 0);
    lv_obj_set_flex_flow(btnContainer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnContainer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(btnContainer, LV_ALIGN_BOTTOM_MID, 0, 0);

    // Helper to create action button with icon
    auto createActionBtn = [&](lv_obj_t** btn, const char* text, IconId iconId, int bufferIdx, lv_event_cb_t callback) {
        *btn = lv_btn_create(btnContainer);
        lv_obj_set_size(*btn, btnWidth, btnHeight);
        lv_obj_set_flex_flow(*btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(*btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(*btn, isSmall ? 1 : (isXLarge ? 4 : 2), 0);
        lv_obj_set_style_pad_row(*btn, isSmall ? 1 : (isXLarge ? 4 : 2), 0);

        // Get the actual button background color from the theme
        lv_color_t btnBgColor = lv_theme_get_color_primary(*btn);

        // Icon canvas with button background color
        lv_obj_t* iconCanvas = lv_canvas_create(*btn);
        lv_canvas_set_buffer(iconCanvas, TamaTac_iconBuffers[bufferIdx], 16, 16, LV_COLOR_FORMAT_NATIVE);
        drawIconWithBg(iconCanvas, iconId, btnBgColor, lv_color_hex(0xFFFFFF));

        // Label
        if (!isSmall) {
            lv_obj_t* label = lv_label_create(*btn);
            lv_label_set_text(label, text);
            lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        }

        lv_obj_add_event_cb(*btn, callback, LV_EVENT_CLICKED, app);
    };

    createActionBtn(&feedBtn, "Feed", ICON_FEED, 7, onFeedClicked);
    createActionBtn(&playBtn, "Play", ICON_PLAY, 8, onPlayClicked);
    createActionBtn(&medicineBtn, "Med", ICON_MEDICINE, 9, onMedicineClicked);
    createActionBtn(&sleepBtn, "Sleep", ICON_SLEEP, 10, onSleepClicked);

    // Refresh timer for periodic UI updates (day/night, random events, etc.)
    refreshTimer = lv_timer_create(onRefreshTimer, 5000, this);

    // Animation timer (~5fps for smooth frame cycling)
    animTimer = lv_timer_create(onAnimTimer, 200, this);
}

void MainView::onStop() {
    if (refreshTimer) {
        lv_timer_del(refreshTimer);
        refreshTimer = nullptr;
    }
    if (animTimer) {
        lv_timer_del(animTimer);
        animTimer = nullptr;
    }
    mainWrapper = nullptr;
    statusLabel = nullptr;
    petArea = nullptr;
    petCanvas = nullptr;
    poopContainer = nullptr;
    for (int i = 0; i < 3; i++) {
        poopIcons[i] = nullptr;
    }
    for (int i = 0; i < 4; i++) {
        statIcons[i] = nullptr;
    }
    hungerBar = nullptr;
    happinessBar = nullptr;
    healthBar = nullptr;
    energyBar = nullptr;
    feedBtn = nullptr;
    playBtn = nullptr;
    medicineBtn = nullptr;
    sleepBtn = nullptr;
    parent = nullptr;
    app = nullptr;
}

void MainView::updateUI(PetLogic* petLogic, LifeStage& lastKnownStage) {
    updateStatBars(petLogic);
    updatePetDisplay(petLogic, lastKnownStage);
}

void MainView::updateStatBars(PetLogic* petLogic) {
    if (petLogic == nullptr) return;

    const PetStats& stats = petLogic->getStats();

    if (hungerBar) lv_bar_set_value(hungerBar, stats.hunger, LV_ANIM_OFF);
    if (happinessBar) lv_bar_set_value(happinessBar, stats.happiness, LV_ANIM_OFF);
    if (healthBar) lv_bar_set_value(healthBar, stats.health, LV_ANIM_OFF);
    if (energyBar) lv_bar_set_value(energyBar, stats.energy, LV_ANIM_OFF);
}

void MainView::updatePetDisplay(PetLogic* petLogic, LifeStage& lastKnownStage) {
    if (petLogic == nullptr) return;

    const PetStats& stats = petLogic->getStats();

    // Check for evolution (Ghost is death, not evolution)
    if (stats.stage != lastKnownStage && statusLabel != nullptr) {
        lastKnownStage = stats.stage;

        if (stats.stage == LifeStage::Ghost) {
            lv_label_set_text(statusLabel, LV_SYMBOL_WARNING " Your pet has died...");
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), LV_SYMBOL_UP " Evolved to %s!", lifeStageToString(stats.stage));
            lv_label_set_text(statusLabel, msg);

            // Trigger evolution flash effect (400ms white flash)
            evolutionFlashUntil = tt::kernel::getMillis() + 400;
        }
    }

    // Update day/night visual
    DayPhase phase = petLogic->getDayPhase();
    currentDayPhase = phase;
    if (petArea) {
        lv_color_t bg = (phase == DayPhase::Night) ? lv_color_hex(0x1a1a2e) : lv_color_hex(0x333333);
        lv_obj_set_style_bg_color(petArea, bg, 0);
    }

    // Update current sprite (animation timer will handle actual drawing)
    SpriteId newSprite = getSpriteForCurrentState(stats);
    if (newSprite != currentSpriteId) {
        currentSpriteId = newSprite;
        animStartTime = tt::kernel::getMillis();
    }
    drawSprite(currentSpriteId, &stats);

    // Update poop display
    for (int i = 0; i < 3; i++) {
        if (poopIcons[i]) {
            if (i < stats.poopCount) {
                drawIcon(poopIcons[i], ICON_POOP);
                lv_obj_clear_flag(poopIcons[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(poopIcons[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
}

void MainView::setStatusText(const char* text) {
    if (statusLabel) {
        lv_label_set_text(statusLabel, text);
    }
}

SpriteId MainView::getSpriteForCurrentState(const PetStats& stats) const {
    if (stats.isDead) {
        return SPRITE_GHOST;
    }

    // Egg always shows egg — no action/mood sprite overrides
    if (stats.stage == LifeStage::Egg) {
        return SPRITE_EGG_IDLE;
    }

    switch (stats.currentAnim) {
        case AnimState::Eating: return SPRITE_EATING;
        case AnimState::Playing: return SPRITE_PLAYING;
        case AnimState::Sleeping: return SPRITE_SLEEPING;
        case AnimState::Sick: return SPRITE_SICK;
        case AnimState::Sad: return SPRITE_SAD;
        default: break;
    }

    if (stats.isSick) {
        return SPRITE_SICK;
    }

    if (stats.isAsleep) {
        return SPRITE_SLEEPING;
    }

    if (stats.happiness >= 70) {
        return SPRITE_HAPPY;
    } else if (stats.happiness < 30) {
        return SPRITE_SAD;
    }

    switch (stats.stage) {
        case LifeStage::Baby: return SPRITE_BABY_IDLE;
        case LifeStage::Teen: return SPRITE_TEEN_IDLE;
        case LifeStage::Adult: return SPRITE_ADULT_IDLE;
        case LifeStage::Elder: return SPRITE_ELDER_IDLE;
        case LifeStage::Ghost: return SPRITE_GHOST;
        default: break;
    }

    return SPRITE_EGG_IDLE;
}

void MainView::drawSprite(SpriteId spriteId, const PetStats* stats) {
    if (petCanvas == nullptr) return;

    uint32_t now = tt::kernel::getMillis();
    int canvasSize = SPRITE_WIDTH * spriteScale;
    if (canvasSize > petCanvasSize) return;

    // Evolution flash: fill entire canvas with white
    if (evolutionFlashUntil > 0 && now < evolutionFlashUntil) {
        lv_canvas_fill_bg(petCanvas, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
        return;
    } else if (evolutionFlashUntil > 0) {
        evolutionFlashUntil = 0;
    }

    // Fill background
    lv_color_t bgColor = (currentDayPhase == DayPhase::Night) ? lv_color_hex(0x1a1a2e) : lv_color_hex(0x333333);
    lv_canvas_fill_bg(petCanvas, bgColor, LV_OPA_COVER);

    // Night stars: draw twinkly dots in the background
    if (currentDayPhase == DayPhase::Night) {
        // Update star seed every 2 seconds for twinkling effect
        uint32_t starPhase = now / 2000;
        uint32_t seed = starPhase * 7919;  // Simple deterministic hash
        for (int i = 0; i < 6; i++) {
            seed = seed * 1103515245 + 12345;  // LCG
            int sx = (seed >> 16) % canvasSize;
            seed = seed * 1103515245 + 12345;
            int sy = (seed >> 16) % (canvasSize / 3);  // Stars only in top third
            // Alternate bright/dim stars
            lv_color_t starColor = (i % 2 == 0) ? lv_color_hex(0xFFFFFF) : lv_color_hex(0x888899);
            lv_canvas_set_px(petCanvas, sx, sy, starColor, LV_OPA_COVER);
        }
    }

    // Determine current animation frame
    const AnimatedSprite& anim = getAnimSprite(spriteId);
    int frameIdx = 0;
    if (anim.frameCount > 1 && anim.frameDelayMs > 0) {
        uint32_t elapsed = now - animStartTime;
        if (anim.loop) {
            frameIdx = (elapsed / anim.frameDelayMs) % anim.frameCount;
        } else {
            frameIdx = elapsed / anim.frameDelayMs;
            if (frameIdx >= anim.frameCount) frameIdx = anim.frameCount - 1;
        }
    }

    // Render RGB565 sprite with transparency and scaling
    const uint16_t* pixelData = anim.frames[frameIdx].data;
    for (int y = 0; y < SPRITE_HEIGHT; y++) {
        for (int x = 0; x < SPRITE_WIDTH; x++) {
            uint16_t pixel = pixelData[y * SPRITE_WIDTH + x];
            if (pixel == SPRITE_TRANSPARENT) continue;

            uint8_t r = (pixel >> 11) << 3;
            uint8_t g = ((pixel >> 5) & 0x3F) << 2;
            uint8_t b = (pixel & 0x1F) << 3;
            lv_color_t color = lv_color_make(r, g, b);

            for (int dy = 0; dy < spriteScale; dy++) {
                for (int dx = 0; dx < spriteScale; dx++) {
                    lv_canvas_set_px(petCanvas,
                        x * spriteScale + dx,
                        y * spriteScale + dy,
                        color, LV_OPA_COVER);
                }
            }
        }
    }

    // Draw overlays (Z's, mood indicator)
    drawOverlays(spriteId, stats);
}

void MainView::drawOverlays(SpriteId spriteId, const PetStats* stats) {
    if (petCanvas == nullptr) return;

    int canvasSize = SPRITE_WIDTH * spriteScale;
    uint32_t now = tt::kernel::getMillis();
    lv_color_t white = lv_color_hex(0xFFFFFF);
    lv_color_t gray = lv_color_hex(0xAAAAAA);

    // Floating Z's during sleep
    if (spriteId == SPRITE_SLEEPING) {
        // Two Z's at different positions, bobbing up and down
        int phase = (now / 500) % 4;  // 4-step bob cycle
        int bobOffset = (phase < 2) ? phase : (4 - phase);  // 0,1,2,1 pattern

        // Large Z (top-right area)
        int zx = canvasSize - 8 * spriteScale / 3;
        int zy = 2 + bobOffset;
        if (zx >= 0 && zx + 3 < canvasSize && zy + 3 < canvasSize) {
            // Z shape: top bar, diagonal, bottom bar
            lv_canvas_set_px(petCanvas, zx, zy, white, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx + 1, zy, white, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx + 2, zy, white, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx + 1, zy + 1, white, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx, zy + 2, white, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx + 1, zy + 2, white, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx + 2, zy + 2, white, LV_OPA_COVER);
        }

        // Small z (offset from large Z, staggered bob)
        int zx2 = zx - 5;
        int bobOffset2 = ((phase + 2) % 4 < 2) ? 0 : 1;
        int zy2 = zy + 4 + bobOffset2;
        if (zx2 >= 0 && zx2 + 2 < canvasSize && zy2 >= 0 && zy2 + 2 < canvasSize) {
            lv_canvas_set_px(petCanvas, zx2, zy2, gray, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx2 + 1, zy2, gray, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx2, zy2 + 1, gray, LV_OPA_COVER);
            lv_canvas_set_px(petCanvas, zx2 + 1, zy2 + 1, gray, LV_OPA_COVER);
        }
    }

    // Mood indicator: small colored dot in bottom-right corner
    if (stats != nullptr && !stats->isDead) {
        lv_color_t moodColor;
        bool showMood = true;

        if (stats->isSick) {
            moodColor = lv_color_hex(0xFF0000);  // Red for sick
        } else if (stats->hunger < 30 || stats->happiness < 30 || stats->energy < 30) {
            moodColor = lv_color_hex(0xFF8800);  // Orange for warning
        } else if (stats->hunger >= 70 && stats->happiness >= 70 && stats->health >= 70 && stats->energy >= 70) {
            moodColor = lv_color_hex(0x00FF00);  // Green for happy
        } else {
            showMood = false;  // Neutral — no indicator
        }

        if (showMood) {
            // 2x2 dot in bottom-right corner
            int mx = canvasSize - 3;
            int my = canvasSize - 3;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    lv_canvas_set_px(petCanvas, mx + dx, my + dy, moodColor, LV_OPA_COVER);
                }
            }
        }
    }
}

void MainView::drawIcon(lv_obj_t* canvas, IconId iconId) {
    drawIconWithBg(canvas, iconId, lv_color_hex(0x333333), lv_color_hex(0xFFFFFF));
}

void MainView::drawIconWithBg(lv_obj_t* canvas, IconId iconId, lv_color_t bgColor, lv_color_t fgColor) {
    if (canvas == nullptr) return;

    const Icon& icon = getIcon(iconId);
    const uint8_t* data = icon.data;

    if (icon.width > 8 || icon.height > 8) return;

    lv_canvas_fill_bg(canvas, bgColor, LV_OPA_COVER);

    const int scale = 2;
    const int width = icon.width;
    const int height = icon.height;

    for (int y = 0; y < height; y++) {
        uint8_t row = data[y];
        for (int x = 0; x < width; x++) {
            bool pixelOn = (row >> (7 - x)) & 1;

            if (pixelOn) {
                for (int dy = 0; dy < scale; dy++) {
                    for (int dx = 0; dx < scale; dx++) {
                        lv_canvas_set_px(canvas, x * scale + dx, y * scale + dy, fgColor, LV_OPA_COVER);
                    }
                }
            }
        }
    }
}

void MainView::onAnimTimer(lv_timer_t* timer) {
    MainView* view = static_cast<MainView*>(lv_timer_get_user_data(timer));
    if (view == nullptr || view->petCanvas == nullptr) return;

    // Pick up deferred reset quickly (200ms vs 5s refresh timer)
    if (TamaTac::pendingResetUI) {
        TamaTac::pendingResetUI = false;
        PetLogic* petLogic = TamaTac::petLogic;
        if (petLogic) {
            view->updateUI(petLogic, TamaTac::lastKnownStage);
        }
        return;
    }

    const PetStats* stats = TamaTac::petLogic ? &TamaTac::petLogic->getStats() : nullptr;
    view->drawSprite(view->currentSpriteId, stats);
}

void MainView::onRefreshTimer(lv_timer_t* timer) {
    MainView* view = static_cast<MainView*>(lv_timer_get_user_data(timer));
    if (view == nullptr || view->app == nullptr) return;

    PetLogic* petLogic = TamaTac::petLogic;
    if (petLogic == nullptr) return;

    view->updateUI(petLogic, TamaTac::lastKnownStage);

    // Display random event messages
    RandomEvent event = petLogic->getLastEvent();
    if (event != RandomEvent::None && view->statusLabel) {
        const char* msg = nullptr;
        switch (event) {
            case RandomEvent::FoundTreat:  msg = "Found a treat!"; break;
            case RandomEvent::MadeFriend:  msg = "Made a friend!"; break;
            case RandomEvent::CaughtCold:  msg = "Caught a cold!"; break;
            case RandomEvent::GotMuddy:    msg = "Got muddy!"; break;
            case RandomEvent::HadNap:      msg = "Had a nap!"; break;
            case RandomEvent::SunnyDay:    msg = "Sunny day!"; break;
            default: break;
        }
        if (msg) {
            lv_label_set_text(view->statusLabel, msg);
        }
        petLogic->clearLastEvent();
    }
}

// Event handlers
void MainView::onFeedClicked(lv_event_t* e) {
    TamaTac* app = static_cast<TamaTac*>(lv_event_get_user_data(e));
    if (app == nullptr) return;
    app->handleFeedAction();
}

void MainView::onPlayClicked(lv_event_t* e) {
    TamaTac* app = static_cast<TamaTac*>(lv_event_get_user_data(e));
    if (app == nullptr) return;
    app->handlePlayAction();
}

void MainView::onMedicineClicked(lv_event_t* e) {
    TamaTac* app = static_cast<TamaTac*>(lv_event_get_user_data(e));
    if (app == nullptr) return;
    app->handleMedicineAction();
}

void MainView::onSleepClicked(lv_event_t* e) {
    TamaTac* app = static_cast<TamaTac*>(lv_event_get_user_data(e));
    if (app == nullptr) return;
    app->handleSleepAction();
}

void MainView::onPetTapped(lv_event_t* e) {
    TamaTac* app = static_cast<TamaTac*>(lv_event_get_user_data(e));
    if (app == nullptr) return;
    app->handlePetTap();
}
