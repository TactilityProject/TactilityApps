/**
 * @file MainView.cpp
 * @brief Main pet view implementation
 */

#include "MainView.h"
#include "Layout.h"
#include "Decor.h"
#include "TamaTac.h"
#include <lvgl_window_manager/window_manager.h>
#include <Tactility/kernel/Kernel.h>
#include <cstdio>
#include <cstdlib>

extern lv_color_t TamaTac_canvasBuffer[72 * 72];
extern lv_color_t TamaTac_iconBuffers[12][16 * 16];

namespace {

SpriteId getSpriteForCurrentState(const PetStats& stats) {
    if (stats.isDead) {
        return SPRITE_GHOST;
    }

    // Egg always shows egg - no action/mood sprite overrides
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

void drawOverlays(MainViewState* state, SpriteId spriteId, const PetStats* stats) {
    if (state->petCanvas == nullptr) return;

    int canvasSize = SPRITE_WIDTH * state->spriteScale;
    uint32_t now = tt::kernel::getMillis();
    lv_color_t white = lv_color_hex(0xFFFFFF);
    lv_color_t gray = lv_color_hex(0xAAAAAA);

    // Floating Z's during sleep
    if (spriteId == SPRITE_SLEEPING) {
        // Two Z's at different positions, bobbing up and down
        int phase = (now / 500) % 4;  // 4-step bob cycle
        int bobOffset = (phase < 2) ? phase : (4 - phase);  // 0,1,2,1 pattern

        // Large Z (top-right area)
        int zx = canvasSize - 8 * state->spriteScale / 3;
        int zy = 2 + bobOffset;
        if (zx >= 0 && zx + 3 < canvasSize && zy + 3 < canvasSize) {
            // Z shape: top bar, diagonal, bottom bar
            lv_canvas_set_px(state->petCanvas, zx, zy, white, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx + 1, zy, white, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx + 2, zy, white, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx + 1, zy + 1, white, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx, zy + 2, white, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx + 1, zy + 2, white, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx + 2, zy + 2, white, LV_OPA_COVER);
        }

        // Small z (offset from large Z, staggered bob)
        int zx2 = zx - 5;
        int bobOffset2 = ((phase + 2) % 4 < 2) ? 0 : 1;
        int zy2 = zy + 4 + bobOffset2;
        if (zx2 >= 0 && zx2 + 2 < canvasSize && zy2 >= 0 && zy2 + 2 < canvasSize) {
            lv_canvas_set_px(state->petCanvas, zx2, zy2, gray, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx2 + 1, zy2, gray, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx2, zy2 + 1, gray, LV_OPA_COVER);
            lv_canvas_set_px(state->petCanvas, zx2 + 1, zy2 + 1, gray, LV_OPA_COVER);
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
            showMood = false;  // Neutral - no indicator
        }

        if (showMood) {
            // 2x2 dot in bottom-right corner
            int mx = canvasSize - 3;
            int my = canvasSize - 3;
            for (int dy = 0; dy < 2; dy++) {
                for (int dx = 0; dx < 2; dx++) {
                    lv_canvas_set_px(state->petCanvas, mx + dx, my + dy, moodColor, LV_OPA_COVER);
                }
            }
        }
    }
}

// Animation frame index for a sprite at a given moment (shared by the draw
// path and the anim-timer skip check so they can never disagree)
int currentFrameIdx(SpriteId spriteId, uint32_t now, uint32_t animStartTime) {
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
    return frameIdx;
}

// Overlay animation phase: sleep Z's bob stepwise, night stars twinkle on a
// slower cycle, everything else is static. Distinct ranges per source.
int overlayPhase(SpriteId spriteId, DayPhase dayPhase, uint32_t now) {
    if (spriteId == SPRITE_SLEEPING) {
        return static_cast<int>((now / 500) % 4);
    }
    if (dayPhase == DayPhase::Night) {
        return 100 + static_cast<int>((now / 2000) % 8);
    }
    return 0;
}

void drawSprite(MainViewState* state, SpriteId spriteId, const PetStats* stats) {
    if (state->petCanvas == nullptr) return;

    uint32_t now = tt::kernel::getMillis();
    int canvasSize = SPRITE_WIDTH * state->spriteScale;
    if (canvasSize > state->petCanvasSize) return;

    bool flashing = (state->evolutionFlashUntil > 0 && now < state->evolutionFlashUntil);

    // Evolution flash: fill entire canvas with white
    if (flashing) {
        lv_canvas_fill_bg(state->petCanvas, lv_color_hex(0xFFFFFF), LV_OPA_COVER);
        state->lastFrameIdx = currentFrameIdx(spriteId, now, state->animStartTime);
        state->lastOverlayPhase = overlayPhase(spriteId, state->currentDayPhase, now);
        state->lastFlash = true;
        state->lastSpriteId = spriteId;
        return;
    } else if (state->evolutionFlashUntil > 0) {
        state->evolutionFlashUntil = 0;
    }

    // Fill background
    lv_color_t bgColor = (state->currentDayPhase == DayPhase::Night) ? lv_color_hex(0x1a1a2e) : lv_color_hex(0x333333);
    lv_canvas_fill_bg(state->petCanvas, bgColor, LV_OPA_COVER);

    // Night stars: draw twinkly dots in the background
    if (state->currentDayPhase == DayPhase::Night) {
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
            lv_canvas_set_px(state->petCanvas, sx, sy, starColor, LV_OPA_COVER);
        }
    }

    // Determine current animation frame
    const AnimatedSprite& anim = getAnimSprite(spriteId);
    int frameIdx = currentFrameIdx(spriteId, now, state->animStartTime);

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

            for (int dy = 0; dy < state->spriteScale; dy++) {
                for (int dx = 0; dx < state->spriteScale; dx++) {
                    lv_canvas_set_px(state->petCanvas,
                        x * state->spriteScale + dx,
                        y * state->spriteScale + dy,
                        color, LV_OPA_COVER);
                }
            }
        }
    }

    // Draw overlays (Z's, mood indicator)
    drawOverlays(state, spriteId, stats);

    // Remember what was drawn so the anim timer can skip identical frames
    state->lastFrameIdx = frameIdx;
    state->lastOverlayPhase = overlayPhase(spriteId, state->currentDayPhase, now);
    state->lastFlash = false;
    state->lastSpriteId = spriteId;
}

void drawIconWithBg(lv_obj_t* canvas, IconId iconId, lv_color_t bgColor, lv_color_t fgColor, int scale) {
    if (canvas == nullptr) return;

    const Icon& icon = getIcon(iconId);
    const uint8_t* data = icon.data;

    if (icon.width > 8 || icon.height > 8) return;

    lv_canvas_fill_bg(canvas, bgColor, LV_OPA_COVER);

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

void drawIcon(lv_obj_t* canvas, IconId iconId, int scale) {
    drawIconWithBg(canvas, iconId, lv_color_hex(0x333333), lv_color_hex(0xFFFFFF), scale);
}

// Icon backing store: heap 24x24 slices on XXL, shared static 16x16 below that
lv_color_t* mainViewIconBuffer(MainViewState* state, int idx) {
    if (state->heapIconBuf != nullptr) {
        return &state->heapIconBuf[idx * 24 * 24];
    }
    return TamaTac_iconBuffers[idx];
}

void onAnimTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    MainViewState* state = &ctx->mainView;
    if (state->petCanvas == nullptr) return;

    // Widgets only exist while this window is topmost - skip otherwise (window_manager deletes
    // a buried window's widgets, but this independent lv_timer_t keeps firing regardless; same
    // reasoning as GPIO.cpp's periodic status timer).
    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) return;

    // Pick up deferred reset quickly (200ms vs 5s refresh timer)
    if (ctx->pendingResetUI) {
        ctx->pendingResetUI = false;
        mainViewUpdateUI(ctx);
        return;
    }

    // Skip the full canvas redraw when nothing would change: at 6-8x scale a
    // redraw is tens of thousands of set_px calls, and most 200ms ticks land
    // on an identical frame (drawSprite refreshes the cache when it draws).
    const uint32_t now = tt::kernel::getMillis();
    const int frameIdx = currentFrameIdx(state->currentSpriteId, now, state->animStartTime);
    const int phase = overlayPhase(state->currentSpriteId, state->currentDayPhase, now);
    const bool flash = (state->evolutionFlashUntil > 0 && now < state->evolutionFlashUntil);
    if (frameIdx == state->lastFrameIdx && phase == state->lastOverlayPhase &&
        flash == state->lastFlash && state->currentSpriteId == state->lastSpriteId) {
        return;
    }

    const PetStats& stats = ctx->petLogic.getStats();
    drawSprite(state, state->currentSpriteId, &stats);
}

void onRefreshTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    MainViewState* state = &ctx->mainView;

    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) return;

    mainViewUpdateUI(ctx);

    // Display random event messages
    RandomEvent event = ctx->petLogic.getLastEvent();
    if (event != RandomEvent::None && state->statusLabel) {
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
            lv_label_set_text(state->statusLabel, msg);
        }
        ctx->petLogic.clearLastEvent();
    }
}

// Event handlers
void onFeedClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    tamaTacHandleFeedAction(ctx);
}

void onPlayClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    tamaTacHandlePlayAction(ctx);
}

void onMedicineClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    tamaTacHandleMedicineAction(ctx);
}

void onSleepClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    tamaTacHandleSleepAction(ctx);
}

void onPetTapped(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr) return;
    tamaTacHandlePetTap(ctx);
}

// Fill-to-fit: grow/shrink the integer pet scale so the column fills the
// wrapper's real content area (called on wrapper SIZE_CHANGED, so it tracks
// rotation/resurface too). Clamped to 2x..8x (8x = 192px, ~73KB heap while
// MainView is active). Small screens return early, keeping their compact
// tier layout untouched.
void mainViewRefitToFill(Context* ctx) {
    MainViewState* state = &ctx->mainView;
    if (!state->mainWrapper || !state->petStatsRow || !state->statsContainer ||
        !state->btnContainer || !state->statusLabel || !state->petArea ||
        !state->petCanvas || !state->poopContainer) {
        return;
    }

    int cw = lv_obj_get_content_width(state->mainWrapper);
    int ch = lv_obj_get_content_height(state->mainWrapper);
    if (cw < 120 || ch < 120) return;  // not laid out yet

    int stH = lv_obj_get_height(state->statusLabel);
    int btnH = lv_obj_get_height(state->btnContainer);
    int poopHt = lv_obj_get_height(state->poopContainer);
    if (stH <= 0 || btnH <= 0 || poopHt < 0) return;

    int fixed = stH + btnH + 2 * state->padRow + 2 * state->padMain;
    int availRow = ch - fixed;
    if (availRow < 40) return;

    int target = state->baseScale;
    if (!state->stacked) {
        int rowW = lv_obj_get_content_width(state->petStatsRow);
        if (rowW < 120) return;
        int sW = (rowW - state->framePad - 8 - 160) / 24;
        int sH = (availRow - state->framePad - poopHt - state->padRow) / 24;
        target = (sW < sH) ? sW : sH;
    } else {
        int statsH = lv_obj_get_height(state->statsContainer);
        if (statsH <= 0) return;
        int sH = (availRow - state->framePad - poopHt - state->padRow - statsH - state->padRow) / 24;
        int sW = (cw - state->framePad) / 24;
        target = (sW < sH) ? sW : sH;
    }

    // No base floor: the tier is only the pre-layout estimate. If the real
    // content area fits less (short-wide screens like the 640x320 simulator),
    // shrink to fit; if it fits more, grow to fill. Small screens return
    // early above, so their compact layout is untouched.
    if (target < 2) target = 2;
    if (target > 8) target = 8;
    if (target == state->spriteScale) return;

    int newCanvas = target * 24;
    lv_color_t* buf = TamaTac_canvasBuffer;
    if (newCanvas > 72) {
        // Allocate the replacement first and only then release the old buffer:
        // a failed realloc would otherwise leave heapCanvas dangling (or force
        // a wasteful copy). The old buffer stays live if this fails.
        const size_t newBytes = static_cast<size_t>(newCanvas) * static_cast<size_t>(newCanvas) * sizeof(lv_color_t);
        lv_color_t* fresh = static_cast<lv_color_t*>(malloc(newBytes));
        if (fresh == nullptr) return;
        if (state->heapCanvas != nullptr) {
            free(state->heapCanvas);
        }
        state->heapCanvas = fresh;
        buf = fresh;
    } else if (state->heapCanvas != nullptr) {
        // Shrinking back to the static buffer: point at it first, then free.
        buf = TamaTac_canvasBuffer;
        free(state->heapCanvas);
        state->heapCanvas = nullptr;
    }

    int newArea = newCanvas + state->framePad;
    state->spriteScale = target;
    state->petCanvasSize = newCanvas;
    lv_obj_set_size(state->petArea, newArea, newArea);
    lv_obj_set_size(state->petCanvas, newCanvas, newCanvas);
    lv_canvas_set_buffer(state->petCanvas, buf, newCanvas, newCanvas, LV_COLOR_FORMAT_NATIVE);
    lv_obj_center(state->petCanvas);
    lv_obj_set_width(state->poopContainer, newArea);
    if (!state->stacked) {
        lv_obj_set_height(state->statsContainer, newArea);
    }
    mainViewUpdatePetDisplay(ctx);
}

// Button row placement: explicit ROW containers, never ROW_WRAP (wrap has
// proven unreliable here: exact-fit rows collapse to a single column). Four
// across in one row, or 2+2 in two rows. Reparents only on mode change -
// re-setting the same parent would reshuffle the order. contW <= 0 means
// percent mode (small/medium/large tiers): full-width single row.
void mainViewPlaceButtons(Context* ctx, bool twoRows, int bw, int contW) {
    MainViewState* state = &ctx->mainView;
    if (!state->btnRowTop || !state->btnRowBottom || !state->btnContainer ||
        !state->feedBtn || !state->playBtn || !state->medicineBtn || !state->sleepBtn) {
        return;
    }
    lv_obj_set_width(state->feedBtn, bw);
    lv_obj_set_width(state->playBtn, bw);
    lv_obj_set_width(state->medicineBtn, bw);
    lv_obj_set_width(state->sleepBtn, bw);
    if (contW <= 0) {
        lv_obj_set_width(state->btnContainer, LV_PCT(100));
        lv_obj_set_width(state->btnRowTop, LV_PCT(100));
    } else {
        lv_obj_set_width(state->btnContainer, contW);
    }
    if (twoRows == state->buttonsTwoRows) {
        // Same mode: refresh widths in case the target changed
        if (contW <= 0) {
            lv_obj_set_width(state->btnContainer, LV_PCT(100));
            lv_obj_set_width(state->btnRowTop, LV_PCT(100));
        } else if (twoRows) {
            // +16px slack: exact-fit rows clip a pixel under theme minimums
            int rowW = 2 * bw + state->btnGap + 16;
            lv_obj_set_width(state->btnRowTop, rowW);
            lv_obj_set_width(state->btnRowBottom, rowW);
        } else {
            lv_obj_set_width(state->btnRowTop, contW);
        }
        return;
    }
    state->buttonsTwoRows = twoRows;
    if (twoRows) {
        int rowW = 2 * bw + state->btnGap + 16;
        lv_obj_set_width(state->btnRowTop, rowW);
        lv_obj_set_width(state->btnRowBottom, rowW);
        lv_obj_set_parent(state->feedBtn, state->btnRowTop);
        lv_obj_set_parent(state->playBtn, state->btnRowTop);
        lv_obj_set_parent(state->medicineBtn, state->btnRowBottom);
        lv_obj_set_parent(state->sleepBtn, state->btnRowBottom);
        lv_obj_set_hidden(state->btnRowBottom, false);
    } else {
        if (contW > 0) {
            lv_obj_set_width(state->btnRowTop, contW);
        }
        lv_obj_set_parent(state->feedBtn, state->btnRowTop);
        lv_obj_set_parent(state->playBtn, state->btnRowTop);
        lv_obj_set_parent(state->medicineBtn, state->btnRowTop);
        lv_obj_set_parent(state->sleepBtn, state->btnRowTop);
        lv_obj_set_hidden(state->btnRowBottom, true);
    }
}
// Width fit: the create-time cap is display-based, but the real window can
// be narrower (portrait Tab5 system panels). Shrink the capped rows to the
// wrapper's actual content width. Percent-mode tiers follow the parent
// automatically and skip this.
//
// Button widths are MEASURED, not assumed: theme minimums can render buttons
// wider than set, and only the rendered width tells whether four fit. The
// layout timer re-runs this until it converges (set widths -> layout
// settles -> measure -> size container from reality).
void mainViewFitWidths(Context* ctx) {
    MainViewState* state = &ctx->mainView;
    if (!state->mainWrapper || !state->petStatsRow || !state->btnContainer ||
        !state->statusLabel || !state->feedBtn || !state->playBtn ||
        !state->medicineBtn || !state->sleepBtn) {
        return;
    }
    if (state->contentPx <= 0) return;

    int cw = lv_obj_get_content_width(state->mainWrapper);
    if (cw < 50) return;  // not laid out yet

    // Live cap: recomputed from the CURRENT display resolution every pass.
    // A create-time cap goes stale across IMU rotation (portrait 688 would
    // pin landscape to 2x2 forever). Shrink to the real window when narrower,
    // restore to the cap when wider again. Unchanged widths no-op in LVGL.
    TamaViewport liveVp = tamaGetViewport();
    int liveCap = tamaContentCap(liveVp);
    if (liveCap > 0) {
        state->contentPx = liveCap;
    }
    int target = (cw < state->contentPx) ? cw : state->contentPx;
    lv_obj_set_width(state->petStatsRow, target);
    lv_obj_set_width(state->statusLabel, target);

    int gap = state->btnGap;
    int measured = lv_obj_get_width(state->feedBtn);
    if (measured < 10) measured = state->baseBtnW;  // pre-layout fallback

    // Four across only with a full gap of breathing room past reality;
    // otherwise explicit 2+2 rows (never wrap).
    bool four = (4 * measured + 3 * gap <= target - gap);
    int bw = state->baseBtnW;
    if (four) {
        int maxFour = (target - 4 * gap) / 4;
        if (maxFour < bw) bw = (maxFour >= 48) ? maxFour : bw;
    }
    // +16px slack: exact-fit rows clip under theme minimums
    int contW = four ? target : 2 * bw + gap + 16;
    mainViewPlaceButtons(ctx, !four, bw, contW);
}

void onMainWrapperResized(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx == nullptr || ctx->activeView != ViewType::Main) return;
    mainViewFitWidths(ctx);
    mainViewRefitToFill(ctx);
}

// Deferred layout passes: SIZE_CHANGED only fires when the wrapper's size
// *changes*, so on first show (sized right from the start) the corrections
// never run. Re-run fit+refit a few times until layout settles instead of
// trusting a single shot (window grant / first layout may lag creation).
void onLayoutTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    MainViewState* state = &ctx->mainView;
    state->layoutTimer = nullptr;
    state->layoutPasses++;
    if (ctx->activeView != ViewType::Main) return;  // navigated away: stop
    bool granted = (window_manager_get_state(ctx->window) == WINDOW_STATE_GRANTED);
    if (granted) {
        // Snapshot to detect a settled layout (snapshot BEFORE fit/refit)
        const int scaleBefore = state->spriteScale;
        const int btnWBefore = (state->btnContainer != nullptr)
            ? lv_obj_get_width(state->btnContainer) : 0;
        const int rowWBefore = (state->petStatsRow != nullptr)
            ? lv_obj_get_width(state->petStatsRow) : 0;
        mainViewFitWidths(ctx);
        mainViewRefitToFill(ctx);
        // Reveal: the wrapper starts invisible so the estimate->measured
        // correction never shows as a resize morph, only as a pop-in.
        lv_obj_set_style_opa(state->mainWrapper, LV_OPA_COVER, 0);
        // Only a fully-laid-out pass counts as calm: both fit and refit bail
        // early (changing nothing) while the wrapper is still unmeasured, and
        // that must not look like convergence or the timer stops too soon.
        const int contentH = (state->mainWrapper != nullptr)
            ? lv_obj_get_content_height(state->mainWrapper) : 0;
        const int contentW = (state->mainWrapper != nullptr)
            ? lv_obj_get_content_width(state->mainWrapper) : 0;
        if (contentW >= 120 && contentH >= 120 &&
            state->spriteScale == scaleBefore &&
            lv_obj_get_width(state->btnContainer) == btnWBefore &&
            lv_obj_get_width(state->petStatsRow) == rowWBefore) {
            state->layoutCalm++;
        } else {
            state->layoutCalm = 0;
        }
    }
    // Stop early once settled (usually pass 2); cap at 5 while the
    // window/grant is still settling (launch can lag creation).
    if (state->layoutCalm < 1 && state->layoutPasses < 5) {
        state->layoutTimer = lv_timer_create(onLayoutTimer, 150, ctx);
        lv_timer_set_repeat_count(state->layoutTimer, 1);
    }
}

} // namespace

// Forward: scenery/background builders (defined with the update fns below)
void createScenery(lv_obj_t* parent, Context* ctx, const TamaViewport& vp);
void mainViewApplyBackground(Context* ctx);

void mainViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx) {
    MainViewState* state = &ctx->mainView;

    // Shared viewport detection (display resolution + orientation)
    TamaViewport vp = tamaGetViewport();
    bool isSmall = tamaIsSmall(vp);
    bool isXLarge = tamaIsXLarge(vp);
    bool isXXLarge = tamaIsXXLarge(vp);
    bool isLarge = !isXLarge && (vp.width >= 400 && vp.height >= 300);
    // Portrait-narrow screens (e.g. 135x240) stack pet above stats;
    // landscape and wide screens keep the side-by-side row.
    bool stackVertical = vp.isPortrait && vp.isNarrow;

    // Scaled dimensions based on screen tier.
    // Static 72x72 buffer covers Small/Medium; Large/XLarge heap-allocate
    // so no-PSRAM devices never pay the extra RAM (see heapCanvas).
    int petAreaSize, statBarWidth, statBarHeight;
    int btnWidth, btnHeight, padMain, padRow, poopHeight;

    if (isSmall) {
        petAreaSize = 56; state->petCanvasSize = 48; state->spriteScale = 2;  // 24*2=48
        statBarWidth = 85; statBarHeight = 5;
        btnWidth = 54; btnHeight = 32;
        padMain = 1; padRow = 1; poopHeight = 14;
        state->iconPx = 16;
    } else if (isXXLarge) {
        petAreaSize = 168; state->petCanvasSize = 144; state->spriteScale = 6;  // 24*6=144
        statBarWidth = 320; statBarHeight = 16;
        btnWidth = 170; btnHeight = 76;
        padMain = 16; padRow = 10; poopHeight = 26;
        state->iconPx = 24;
    } else if (isXLarge) {
        petAreaSize = 168; state->petCanvasSize = 144; state->spriteScale = 6;  // 24*6=144
        statBarWidth = 300; statBarHeight = 12;
        btnWidth = 150; btnHeight = 64;
        padMain = 12; padRow = 8; poopHeight = 24;
        state->iconPx = 16;
    } else if (isLarge) {
        petAreaSize = 112; state->petCanvasSize = 96; state->spriteScale = 4;  // 24*4=96
        statBarWidth = 200; statBarHeight = 8;
        btnWidth = 90; btnHeight = 50;
        padMain = 4; padRow = 4; poopHeight = 22;
        state->iconPx = 16;
    } else {
        // Medium (default)
        petAreaSize = 76; state->petCanvasSize = 72; state->spriteScale = 3;
        statBarWidth = 135; statBarHeight = 6;
        btnWidth = 70; btnHeight = 42;
        padMain = 1; padRow = 1; poopHeight = 16;
        state->iconPx = 16;
    }

    // Canvas backing store: shared static buffer up to 72px, heap beyond that
    lv_color_t* canvasBuf = TamaTac_canvasBuffer;
    if (state->petCanvasSize > 72) {
        state->heapCanvas = static_cast<lv_color_t*>(
            malloc(static_cast<size_t>(state->petCanvasSize) * static_cast<size_t>(state->petCanvasSize) * sizeof(lv_color_t)));
        if (state->heapCanvas != nullptr) {
            canvasBuf = state->heapCanvas;
        } else {
            // OOM fallback: stay within the static buffer, keeping the tier's
            // frame padding so petAreaSize/framePad stay consistent (a 168px
            // area around a 72px canvas would misplace everything downstream).
            int frame = petAreaSize - state->petCanvasSize;
            state->petCanvasSize = 72;
            state->spriteScale = 3;
            petAreaSize = 72 + frame;
        }
    }

    // Icon backing store: 24x24 heap slices on XXL, shared static 16x16 below
    if (state->iconPx > 16) {
        state->heapIconBuf = static_cast<lv_color_t*>(
            malloc(12U * 24U * 24U * sizeof(lv_color_t)));
        if (state->heapIconBuf == nullptr) {
            state->iconPx = 16;  // OOM fallback: small icons
        }
    }
    const int iconScale = state->iconPx / 8;  // 2 or 3
    state->baseScale = state->spriteScale;
    state->framePad = petAreaSize - state->petCanvasSize;
    state->padMain = padMain;
    state->padRow = padRow;
    state->poopH = poopHeight;
    state->stacked = stackVertical;

    // Cap content width on big screens so bars/buttons don't stretch absurdly
    // (e.g. 1280px Tab5 landscape -> 800px centered column, 800x480 -> 720px).
    // Live recompute on every fit (rotation changes vp) - see fitWidths.
    int contentWidthPct = 98;
    int contentWidthPx = tamaContentCap(vp);

    // Button metrics. baseBtnW keeps the tier width so rotation back to
    // landscape restores full size; placement happens after the buttons
    // exist (mainViewPlaceButtons below) and re-runs post-layout.
    state->btnGap = isSmall ? 3 : (isXLarge ? 16 : 6);
    state->baseBtnW = btnWidth;
    state->contentPx = contentWidthPx;

    // Initialize animation state
    state->animStartTime = tt::kernel::getMillis();
    state->currentSpriteId = SPRITE_EGG_IDLE;
    // Fresh draw cache: stale values from a previous view lifetime would make
    // the anim timer wrongly skip the first draws
    state->lastFrameIdx = -1;
    state->lastOverlayPhase = -1;
    state->lastFlash = false;
    state->lastSpriteId = SPRITE_EGG_IDLE;

    // Main content wrapper (vertically centered on big screens so the
    // pet block doesn't cling to the toolbar leaving half the screen empty).
    // Backdrop is a day/night vertical gradient (free fill, no image layer);
    // scenery decor parents to the wrapper first so it paints behind content.
    // bgApplied resets so fresh objects get styled on first update.
    state->bgApplied = false;
    state->mainWrapper = lv_obj_create(parentWidget);
    lv_obj_set_size(state->mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_pad_all(state->mainWrapper, padMain, 0);
    lv_obj_set_style_bg_opa(state->mainWrapper, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(state->mainWrapper, lv_color_hex(0x4760B8), LV_PART_MAIN);
    lv_obj_set_style_border_width(state->mainWrapper, 0, 0);
    lv_obj_set_flex_flow(state->mainWrapper, LV_FLEX_FLOW_COLUMN);
    if (isXLarge) {
        lv_obj_set_flex_align(state->mainWrapper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_set_flex_align(state->mainWrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    }
    lv_obj_set_scrollable(state->mainWrapper, false);
    // Refit pet scale once the real content area is known (and on rotation)
    lv_obj_add_event_cb(state->mainWrapper, onMainWrapperResized, LV_EVENT_SIZE_CHANGED, ctx);

    // Scenery first-children: sun/moon/clouds/ground paint above the gradient
    // but below all content (XL/XXLarge only, decided inside)
    createScenery(state->mainWrapper, ctx, vp);

    // Pet and stats container: side-by-side row normally,
    // stacked column on portrait-narrow screens (135x240 etc.)
    lv_obj_t* petStatsRow = lv_obj_create(state->mainWrapper);
    state->petStatsRow = petStatsRow;
    if (contentWidthPx > 0) {
        lv_obj_set_size(petStatsRow, contentWidthPx, LV_SIZE_CONTENT);
    } else {
        lv_obj_set_size(petStatsRow, LV_PCT(contentWidthPct), LV_SIZE_CONTENT);
    }
    lv_obj_set_style_bg_opa(petStatsRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(petStatsRow, 0, 0);
    lv_obj_set_style_pad_all(petStatsRow, 0, 0);
    lv_obj_set_style_pad_left(petStatsRow, isSmall ? 2 : (isXLarge ? 16 : 8), 0);
    if (stackVertical) {
        lv_obj_set_flex_flow(petStatsRow, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(petStatsRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    } else {
        lv_obj_set_flex_flow(petStatsRow, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(petStatsRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    }

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
    state->petArea = lv_obj_create(petColumn);
    lv_obj_set_size(state->petArea, petAreaSize, petAreaSize);
    lv_obj_set_style_bg_color(state->petArea, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_color(state->petArea, lv_color_hex(0x666666), 0);
    lv_obj_set_style_border_width(state->petArea, isSmall ? 1 : (isXLarge ? 3 : 2), 0);
    lv_obj_set_style_pad_all(state->petArea, 0, 0);

    // Make pet area tappable for pet interaction
    lv_obj_set_hidden(state->petArea, false);
    lv_obj_add_event_cb(state->petArea, onPetTapped, LV_EVENT_CLICKED, ctx);

    // Create canvas for sprite rendering
    state->petCanvas = lv_canvas_create(state->petArea);
    lv_obj_set_size(state->petCanvas, state->petCanvasSize, state->petCanvasSize);
    lv_canvas_set_buffer(state->petCanvas, canvasBuf, state->petCanvasSize, state->petCanvasSize, LV_COLOR_FORMAT_NATIVE);
    lv_obj_center(state->petCanvas);

    // Poop container
    state->poopContainer = lv_obj_create(petColumn);
    lv_obj_set_size(state->poopContainer, petAreaSize, poopHeight);
    lv_obj_set_style_bg_opa(state->poopContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(state->poopContainer, 0, 0);
    lv_obj_set_style_pad_all(state->poopContainer, 1, 0);
    lv_obj_set_style_pad_column(state->poopContainer, isSmall ? 1 : (isXLarge ? 4 : 2), 0);
    lv_obj_set_flex_flow(state->poopContainer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(state->poopContainer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(state->poopContainer, false);

    // Create 3 poop icon canvases
    for (int i = 0; i < 3; i++) {
        state->poopIcons[i] = lv_canvas_create(state->poopContainer);
        lv_canvas_set_buffer(state->poopIcons[i], mainViewIconBuffer(state, i), state->iconPx, state->iconPx, LV_COLOR_FORMAT_NATIVE);
        lv_obj_set_hidden(state->poopIcons[i], true);
    }

    // Stat bars container (grows to fill remaining space in both orientations)
    lv_obj_t* statsContainer = lv_obj_create(petStatsRow);
    state->statsContainer = statsContainer;
    if (stackVertical) {
        lv_obj_set_size(statsContainer, LV_PCT(100), LV_SIZE_CONTENT);
    } else {
        lv_obj_set_size(statsContainer, LV_SIZE_CONTENT, petAreaSize);
    }
    lv_obj_set_style_bg_opa(statsContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(statsContainer, 0, 0);
    lv_obj_set_style_pad_all(statsContainer, padRow, 0);
    lv_obj_set_style_pad_left(statsContainer, isSmall ? 4 : (isXLarge ? 16 : 8), 0);
    lv_obj_set_style_pad_row(statsContainer, padRow, 0);
    lv_obj_set_flex_flow(statsContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(statsContainer, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_flex_grow(statsContainer, 1);
    lv_obj_set_scrollable(statsContainer, false);

    // Helper to create stat row (bar flex-grows; statBarWidth is the minimum)
    auto createStatRow = [&](lv_obj_t* parent, int iconBufferIdx, IconId iconId,
                             lv_obj_t** bar, lv_color_t barColor) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_column(row, isSmall ? 2 : (isXLarge ? 6 : 3), 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        lv_obj_t* iconCanvas = lv_canvas_create(row);
        lv_canvas_set_buffer(iconCanvas, mainViewIconBuffer(state, iconBufferIdx), state->iconPx, state->iconPx, LV_COLOR_FORMAT_NATIVE);
        drawIcon(iconCanvas, iconId, iconScale);

        *bar = lv_bar_create(row);
        lv_obj_set_size(*bar, statBarWidth, statBarHeight);
        lv_obj_set_flex_grow(*bar, 1);
        lv_bar_set_range(*bar, 0, 100);
        lv_obj_set_style_bg_color(*bar, lv_color_hex(0x444444), LV_PART_MAIN);
        lv_obj_set_style_bg_color(*bar, barColor, LV_PART_INDICATOR);

        return iconCanvas;
    };

    state->statIcons[0] = createStatRow(statsContainer, 3, ICON_HUNGER, &state->hungerBar, lv_color_hex(0xFF9900));
    state->statIcons[1] = createStatRow(statsContainer, 4, ICON_HAPPINESS, &state->happinessBar, lv_color_hex(0xFFCC00));
    state->statIcons[2] = createStatRow(statsContainer, 5, ICON_HEALTH, &state->healthBar, lv_color_hex(0x00FF00));
    state->statIcons[3] = createStatRow(statsContainer, 6, ICON_ENERGY, &state->energyBar, lv_color_hex(0x00CCFF));

    // Status label (wraps on narrow screens instead of overflowing)
    state->statusLabel = lv_label_create(state->mainWrapper);
    lv_label_set_text(state->statusLabel, "Your pet is ready!");
    if (contentWidthPx > 0) {
        lv_obj_set_width(state->statusLabel, contentWidthPx);
    } else {
        lv_obj_set_width(state->statusLabel, LV_PCT(100));
    }
    lv_label_set_long_mode(state->statusLabel, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(state->statusLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_top(state->statusLabel, 0, 0);
    lv_obj_set_style_pad_bottom(state->statusLabel, 0, 0);

    // Button container: explicit rows, never wrap (see mainViewPlaceButtons)
    lv_obj_t* btnContainer = lv_obj_create(state->mainWrapper);
    state->btnContainer = btnContainer;
    // Width is set by mainViewPlaceButtons below
    lv_obj_set_height(btnContainer, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btnContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnContainer, 0, 0);
    lv_obj_set_style_pad_all(btnContainer, 0, 0);
    lv_obj_set_style_pad_row(btnContainer, isSmall ? 3 : (isXLarge ? 16 : 6), 0);
    lv_obj_set_style_pad_top(btnContainer, padRow, 0);
    lv_obj_set_flex_flow(btnContainer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(btnContainer, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(btnContainer, false);

    auto createBtnRow = [&](lv_obj_t** row) {
        *row = lv_obj_create(btnContainer);
        lv_obj_set_size(*row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(*row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(*row, 0, 0);
        lv_obj_set_style_pad_all(*row, 0, 0);
        lv_obj_set_style_pad_column(*row, isSmall ? 3 : (isXLarge ? 16 : 6), 0);
        lv_obj_set_flex_flow(*row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(*row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_scrollable(*row, false);
    };
    createBtnRow(&state->btnRowTop);
    createBtnRow(&state->btnRowBottom);
    lv_obj_set_hidden(state->btnRowBottom, true);  // shown in 2x2 mode
    state->buttonsTwoRows = false;

    // Helper to create action button with icon (all start in the top row;
    // mainViewPlaceButtons moves pairs down for 2x2 after measuring)
    auto createActionBtn = [&](lv_obj_t** btn, const char* text, IconId iconId, int bufferIdx, lv_event_cb_t callback) {
        *btn = lv_btn_create(state->btnRowTop);
        lv_obj_set_size(*btn, btnWidth, btnHeight);
        // Our explicit widths must hold exactly: neutralize any theme button
        // min-width, otherwise rows compute against a wider button than set
        // and wrap unpredictably (portrait 3+1 / 1-column symptoms).
        lv_obj_set_style_min_width(*btn, 0, LV_PART_MAIN);
        lv_obj_set_flex_flow(*btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(*btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(*btn, isSmall ? 1 : (isXLarge ? 4 : 2), 0);
        lv_obj_set_style_pad_row(*btn, isSmall ? 1 : (isXLarge ? 4 : 2), 0);

        // Get the actual button background color from the theme
        lv_color_t btnBgColor = lv_theme_get_color_primary(*btn);

        // Icon canvas with button background color
        lv_obj_t* iconCanvas = lv_canvas_create(*btn);
        lv_canvas_set_buffer(iconCanvas, mainViewIconBuffer(state, bufferIdx), state->iconPx, state->iconPx, LV_COLOR_FORMAT_NATIVE);
        drawIconWithBg(iconCanvas, iconId, btnBgColor, lv_color_hex(0xFFFFFF), iconScale);

        // Label (hidden only on short landscape-small screens like 240x135
        // where vertical space is critical; kept in portrait-narrow like
        // 135x240 where buttons wrap 2x2 and height allows it)
        bool showLabel = !(isSmall && vp.isShort && !stackVertical);
        if (showLabel) {
            lv_obj_t* label = lv_label_create(*btn);
            lv_label_set_text(label, text);
            lv_obj_set_style_text_font(label, lv_font_get_default(), 0);
        }

        lv_obj_add_event_cb(*btn, callback, LV_EVENT_CLICKED, ctx);
    };

    createActionBtn(&state->feedBtn, "Feed", ICON_FEED, 7, onFeedClicked);
    createActionBtn(&state->playBtn, "Play", ICON_PLAY, 8, onPlayClicked);
    createActionBtn(&state->medicineBtn, "Med", ICON_MEDICINE, 9, onMedicineClicked);
    createActionBtn(&state->sleepBtn, "Sleep", ICON_SLEEP, 10, onSleepClicked);

    // Initial button placement (display-based estimate; fitWidths corrects
    // post-layout from measured widths)
    if (contentWidthPx > 0) {
        bool two = (4 * btnWidth + 3 * state->btnGap > contentWidthPx - state->btnGap);
        int initCont = two ? 2 * btnWidth + state->btnGap + 16 : contentWidthPx;
        mainViewPlaceButtons(ctx, two, btnWidth, initCont);
    } else {
        mainViewPlaceButtons(ctx, false, btnWidth, 0);
    }

    // Refresh timer for periodic UI updates (day/night, random events, etc.)
    state->refreshTimer = lv_timer_create(onRefreshTimer, 5000, ctx);

    // Animation timer (~5fps for smooth frame cycling)
    state->animTimer = lv_timer_create(onAnimTimer, 200, ctx);

    // Deferred layout correction (see onLayoutTimer). The wrapper starts
    // fully transparent so the estimate->measured correction appears as a
    // single pop-in rather than a visible resize morph; the first granted
    // pass restores opacity. Transparency inherits (layout still runs).
    lv_obj_set_style_opa(state->mainWrapper, LV_OPA_TRANSP, 0);
    state->layoutPasses = 0;
    state->layoutCalm = 0;
    state->layoutTimer = lv_timer_create(onLayoutTimer, 100, ctx);
    lv_timer_set_repeat_count(state->layoutTimer, 1);
}

void mainViewStop(Context* ctx) {
    MainViewState* state = &ctx->mainView;
    if (state->refreshTimer) {
        lv_timer_del(state->refreshTimer);
        state->refreshTimer = nullptr;
    }
    if (state->animTimer) {
        lv_timer_del(state->animTimer);
        state->animTimer = nullptr;
    }
    if (state->layoutTimer) {
        lv_timer_del(state->layoutTimer);
        state->layoutTimer = nullptr;
    }
    if (state->heapCanvas) {
        free(state->heapCanvas);
        state->heapCanvas = nullptr;
    }
    if (state->heapIconBuf) {
        free(state->heapIconBuf);
        state->heapIconBuf = nullptr;
    }
    state->mainWrapper = nullptr;
    state->petStatsRow = nullptr;
    state->statsContainer = nullptr;
    state->btnContainer = nullptr;
    state->btnRowTop = nullptr;
    state->btnRowBottom = nullptr;
    state->buttonsTwoRows = false;
    state->bgStrip = nullptr;
    state->bgSun = nullptr;
    state->bgMoon = nullptr;
    state->bgCloud1 = nullptr;
    state->bgCloud2 = nullptr;
    state->bgCloud3 = nullptr;
    state->statusLabel = nullptr;
    state->petArea = nullptr;
    state->petCanvas = nullptr;
    state->poopContainer = nullptr;
    for (int i = 0; i < 3; i++) {
        state->poopIcons[i] = nullptr;
    }
    for (int i = 0; i < 4; i++) {
        state->statIcons[i] = nullptr;
    }
    state->hungerBar = nullptr;
    state->happinessBar = nullptr;
    state->healthBar = nullptr;
    state->energyBar = nullptr;
    state->feedBtn = nullptr;
    state->playBtn = nullptr;
    state->medicineBtn = nullptr;
    state->sleepBtn = nullptr;
}

void mainViewUpdateUI(Context* ctx) {
    mainViewUpdateStatBars(ctx);
    mainViewUpdatePetDisplay(ctx);
}

void mainViewUpdateStatBars(Context* ctx) {
    MainViewState* state = &ctx->mainView;
    const PetStats& stats = ctx->petLogic.getStats();

    if (state->hungerBar) lv_bar_set_value(state->hungerBar, stats.hunger, LV_ANIM_OFF);
    if (state->happinessBar) lv_bar_set_value(state->happinessBar, stats.happiness, LV_ANIM_OFF);
    if (state->healthBar) lv_bar_set_value(state->healthBar, stats.health, LV_ANIM_OFF);
    if (state->energyBar) lv_bar_set_value(state->energyBar, stats.energy, LV_ANIM_OFF);
}

// Scenery overlays (XL/XXL only): decor behind the content.
// All floating+anchored, so rotation tracks with zero resize code and the
// flex column math is untouched. Created before mainWrapper => behind it.
void createScenery(lv_obj_t* parent, Context* ctx, const TamaViewport& vp) {
    MainViewState* state = &ctx->mainView;

    // NOTE: parent is mainWrapper itself, and these are its FIRST children so
    // they paint above the wrapper background but below all content. All are
    // floating (out of the flex flow) and anchored (track resizes for free).

    // Sun/moon/clouds/ground need room to breathe: XL/XXL only
    if (!tamaIsXLarge(vp)) return;
    auto makeOrb = [&](lv_obj_t** out, const lv_image_dsc_t* dsc,
                       lv_color_t tint, lv_opa_t fade) {
        *out = lv_image_create(parent);
        lv_image_set_src(*out, dsc);
        lv_obj_set_style_image_recolor(*out, tint, LV_PART_MAIN);
        lv_obj_set_style_image_recolor_opa(*out, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_image_opa(*out, fade, LV_PART_MAIN);
        lv_obj_set_floating(*out, true);
        lv_obj_set_clickable(*out, false);
    };

    // Ground strip: full-bleed tiled grass pinned to the bottom
    state->bgStrip = lv_obj_create(parent);
    lv_obj_set_size(state->bgStrip, LV_PCT(100), 24);
    lv_obj_set_style_pad_all(state->bgStrip, 0, 0);
    lv_obj_set_style_border_width(state->bgStrip, 0, 0);
    lv_obj_set_style_radius(state->bgStrip, 0, 0);
    lv_obj_set_style_bg_image_src(state->bgStrip, &grassDayDsc, LV_PART_MAIN);
    lv_obj_set_style_bg_image_tiled(state->bgStrip, true, LV_PART_MAIN);
    lv_obj_set_style_bg_image_opa(state->bgStrip, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_floating(state->bgStrip, true);
    lv_obj_set_clickable(state->bgStrip, false);
    lv_obj_set_scrollable(state->bgStrip, false);
    lv_obj_align(state->bgStrip, LV_ALIGN_BOTTOM_MID, 0, 0);

    makeOrb(&state->bgSun, &decorSunDsc, lv_color_hex(0xFFD94A), 200);
    lv_obj_align(state->bgSun, LV_ALIGN_TOP_RIGHT, -20, 20);

    makeOrb(&state->bgMoon, &decorMoonDsc, lv_color_hex(0xE8E8F5), 200);
    lv_obj_align(state->bgMoon, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_set_hidden(state->bgMoon, true);  // day default; apply fixes

    makeOrb(&state->bgCloud1, &cloudBigDsc, lv_color_hex(0xFFFFFF), 110);
    lv_obj_align(state->bgCloud1, LV_ALIGN_TOP_LEFT,
        vp.width * 5 / 100, vp.height * 12 / 100);

    makeOrb(&state->bgCloud2, &cloudSmallDsc, lv_color_hex(0xFFFFFF), 110);
    lv_obj_align(state->bgCloud2, LV_ALIGN_TOP_LEFT,
        vp.width * 40 / 100, vp.height * 6 / 100);

    makeOrb(&state->bgCloud3, &cloudSmallDsc, lv_color_hex(0xFFFFFF), 90);
    lv_obj_align(state->bgCloud3, LV_ALIGN_TOP_LEFT,
        vp.width * 72 / 100, vp.height * 16 / 100);
}

// Day/night backdrop: flat colors (RGB565 gradients band visibly and LVGL
// has no dither pass; flat is fastest and band-free) plus the XL/XXL scenery
// set. Swapped only on flips.
void mainViewApplyBackground(Context* ctx) {
    MainViewState* state = &ctx->mainView;
    if (state->mainWrapper == nullptr) return;
    bool night = (state->currentDayPhase == DayPhase::Night);
    if (state->bgApplied && night == state->bgIsNight) return;
    lv_obj_set_style_bg_color(state->mainWrapper,
        lv_color_hex(night ? 0x121839 : 0x4760B8), LV_PART_MAIN);
    if (state->bgStrip != nullptr) {
        lv_obj_set_style_bg_image_src(state->bgStrip,
            night ? &grassNightDsc : &grassDayDsc, LV_PART_MAIN);
    }
    if (state->bgSun != nullptr) {
        if (night) {
            lv_obj_set_hidden(state->bgSun, true);
            lv_obj_set_hidden(state->bgCloud1, true);
            lv_obj_set_hidden(state->bgCloud2, true);
            lv_obj_set_hidden(state->bgCloud3, true);
            lv_obj_set_hidden(state->bgMoon, false);
        } else {
            lv_obj_set_hidden(state->bgSun, false);
            lv_obj_set_hidden(state->bgCloud1, false);
            lv_obj_set_hidden(state->bgCloud2, false);
            lv_obj_set_hidden(state->bgCloud3, false);
            lv_obj_set_hidden(state->bgMoon, true);
        }
    }
    state->bgIsNight = night;
    state->bgApplied = true;
}

void mainViewUpdatePetDisplay(Context* ctx) {
    MainViewState* state = &ctx->mainView;
    const PetStats& stats = ctx->petLogic.getStats();

    // Check for evolution (Ghost is death, not evolution)
    if (stats.stage != ctx->lastKnownStage && state->statusLabel != nullptr) {
        ctx->lastKnownStage = stats.stage;

        if (stats.stage == LifeStage::Ghost) {
            lv_label_set_text(state->statusLabel, LV_SYMBOL_WARNING " Your pet has died...");
        } else {
            char msg[64];
            snprintf(msg, sizeof(msg), LV_SYMBOL_UP " Evolved to %s!", lifeStageToString(stats.stage));
            lv_label_set_text(state->statusLabel, msg);

            // Trigger evolution flash effect (400ms white flash)
            state->evolutionFlashUntil = tt::kernel::getMillis() + 400;
        }
    }

    // Update day/night visual
    DayPhase phase = ctx->petLogic.getDayPhase();
    state->currentDayPhase = phase;
    if (state->petArea) {
        lv_color_t bg = (phase == DayPhase::Night) ? lv_color_hex(0x1a1a2e) : lv_color_hex(0x333333);
        lv_obj_set_style_bg_color(state->petArea, bg, 0);
    }
    mainViewApplyBackground(ctx);

    // Update current sprite (animation timer will handle actual drawing)
    SpriteId newSprite = getSpriteForCurrentState(stats);
    if (newSprite != state->currentSpriteId) {
        state->currentSpriteId = newSprite;
        state->animStartTime = tt::kernel::getMillis();
    }
    drawSprite(state, state->currentSpriteId, &stats);

    // Update poop display
    for (int i = 0; i < 3; i++) {
        if (state->poopIcons[i]) {
            if (i < stats.poopCount) {
                drawIcon(state->poopIcons[i], ICON_POOP, state->iconPx / 8);
                lv_obj_set_hidden(state->poopIcons[i], false);
            } else {
                lv_obj_set_hidden(state->poopIcons[i], true);
            }
        }
    }
}

void mainViewSetStatusText(Context* ctx, const char* text) {
    if (ctx->mainView.statusLabel) {
        lv_label_set_text(ctx->mainView.statusLabel, text);
    }
}
