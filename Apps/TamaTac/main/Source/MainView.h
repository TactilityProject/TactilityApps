/**
 * @file MainView.h
 * @brief Main pet view for TamaTac
 */
#pragma once

#include <lvgl.h>
#include "PetLogic.h"
#include "Sprites.h"

struct Context;

struct MainViewState {
    // UI elements
    lv_obj_t* mainWrapper = nullptr;
    lv_obj_t* petStatsRow = nullptr;
    lv_obj_t* statsContainer = nullptr;
    lv_obj_t* btnContainer = nullptr;
    lv_obj_t* btnRowTop = nullptr;
    lv_obj_t* btnRowBottom = nullptr;
    bool buttonsTwoRows = false;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* petArea = nullptr;
    lv_obj_t* petCanvas = nullptr;
    lv_obj_t* poopContainer = nullptr;
    lv_obj_t* poopIcons[3] = {nullptr, nullptr, nullptr};
    lv_obj_t* statIcons[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* hungerBar = nullptr;
    lv_obj_t* happinessBar = nullptr;
    lv_obj_t* healthBar = nullptr;
    lv_obj_t* energyBar = nullptr;
    lv_obj_t* feedBtn = nullptr;
    lv_obj_t* playBtn = nullptr;
    lv_obj_t* medicineBtn = nullptr;
    lv_obj_t* sleepBtn = nullptr;

    // Refresh timer for periodic UI updates
    lv_timer_t* refreshTimer = nullptr;
    DayPhase currentDayPhase = DayPhase::Day;

    // Animation
    lv_timer_t* animTimer = nullptr;
    // One-shot deferred layout correction (deleted after firing / in stop)
    lv_timer_t* layoutTimer = nullptr;
    // Deferred layout pass counters (state survives rebuilds: reset at create)
    int layoutPasses = 0;
    int layoutCalm = 0;
    // Last-drawn frame cache: the 200ms anim timer skips the full canvas
    // redraw when neither the sprite frame nor an overlay phase changed
    int lastFrameIdx = -1;
    int lastOverlayPhase = -1;
    bool lastFlash = false;
    SpriteId lastSpriteId = SPRITE_EGG_IDLE;
    uint32_t animStartTime = 0;
    SpriteId currentSpriteId = SPRITE_EGG_IDLE;

    // Polish effects
    uint32_t evolutionFlashUntil = 0;  // Millis timestamp when flash ends (0 = no flash)
    uint32_t starSeed = 0;            // Seed for deterministic star positions (changes slowly)

    // Scaling
    int spriteScale = 3;
    int petCanvasSize = 72;
    // Heap canvas for scales above the shared 72x72 static buffer
    // (Large 96px / XLarge 144px, plus fill-to-fit growth up to 192px).
    // Null when the static buffer is used, so small no-PSRAM devices
    // never pay the extra RAM.
    lv_color_t* heapCanvas = nullptr;
    // Heap icon block (12 x 24x24) for the XXL 3x icons. Null otherwise,
    // so only Tab5-class devices pay the extra ~14KB while MainView runs.
    lv_color_t* heapIconBuf = nullptr;
    // Icon canvas edge in px (16, or 24 on XXL) and its pixel-doubling scale.
    int iconPx = 16;
    // Tiled background day/night state
    bool bgIsNight = false;
    bool bgApplied = false;
    // Scenery overlays (XL/XXL only, behind content)
    lv_obj_t* bgStrip = nullptr;
    lv_obj_t* bgSun = nullptr;
    lv_obj_t* bgMoon = nullptr;
    lv_obj_t* bgCloud1 = nullptr;
    lv_obj_t* bgCloud2 = nullptr;
    lv_obj_t* bgCloud3 = nullptr;
    // Fill-to-fit bookkeeping (tier base + layout metrics for refit)
    int padMain = 0;
    int padRow = 0;
    int poopH = 0;
    int framePad = 0;  // petAreaSize - petCanvasSize at tier base
    int baseScale = 3;
    bool stacked = false;
    // Width bookkeeping: fixed cap applied at create (0 = percent mode),
    // button base width + gap for post-layout shrinking to the real window.
    int contentPx = 0;
    int baseBtnW = 0;
    int btnGap = 6;
};

void mainViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx);
void mainViewStop(Context* ctx);

void mainViewUpdateUI(Context* ctx);
void mainViewUpdateStatBars(Context* ctx);
void mainViewUpdatePetDisplay(Context* ctx);
void mainViewSetStatusText(Context* ctx, const char* text);
