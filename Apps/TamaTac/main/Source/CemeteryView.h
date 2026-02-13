/**
 * @file CemeteryView.h
 * @brief Pet Cemetery / Hall of Fame view for TamaTac
 */
#pragma once

#include <lvgl.h>
#include <cstdint>
#include "PetStats.h"

class TamaTac;

// Record of a deceased pet
struct PetRecord {
    Personality personality;
    LifeStage stageReached;
    uint16_t ageHours;
    bool valid = false;
};

class CemeteryView {
public:
    static constexpr int MAX_RECORDS = 5;

private:
    TamaTac* app = nullptr;
    lv_obj_t* parent = nullptr;
    lv_obj_t* mainWrapper = nullptr;


public:
    void onStart(lv_obj_t* parentWidget, TamaTac* appInstance);
    void onStop();

    // Record a pet death (called from TamaTac when pet dies)
    static void recordDeath(Personality personality, LifeStage stage, uint16_t ageHours);

    // Load all records
    static void loadRecords(PetRecord records[MAX_RECORDS]);
};
