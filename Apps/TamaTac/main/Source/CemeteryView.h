/**
 * @file CemeteryView.h
 * @brief Pet Cemetery / Hall of Fame view for TamaTac
 */
#pragma once

#include <lvgl.h>
#include <cstdint>
#include "PetStats.h"

struct Context;

// Record of a deceased pet
struct PetRecord {
    Personality personality;
    LifeStage stageReached;
    uint16_t ageHours;
    bool valid = false;
};

constexpr int CEMETERY_MAX_RECORDS = 5;

struct CemeteryViewState {
    lv_obj_t* mainWrapper = nullptr;
};

void cemeteryViewCreateWidgets(lv_obj_t* parentWidget, Context* ctx);
void cemeteryViewStop(Context* ctx);

// Record a pet death (called from TamaTac when pet dies)
void cemeteryViewRecordDeath(Personality personality, LifeStage stage, uint16_t ageHours);

// Load all records
void cemeteryViewLoadRecords(PetRecord records[CEMETERY_MAX_RECORDS]);
