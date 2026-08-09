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
    uint32_t animStartTime = 0;
    SpriteId currentSpriteId = SPRITE_EGG_IDLE;

    // Polish effects
    uint32_t evolutionFlashUntil = 0;  // Millis timestamp when flash ends (0 = no flash)
    uint32_t starSeed = 0;            // Seed for deterministic star positions (changes slowly)

    // Scaling
    int spriteScale = 3;
    int petCanvasSize = 72;
};

void mainViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx);
void mainViewStop(Context* ctx);

void mainViewUpdateUI(Context* ctx);
void mainViewUpdateStatBars(Context* ctx);
void mainViewUpdatePetDisplay(Context* ctx);
void mainViewSetStatusText(Context* ctx, const char* text);
