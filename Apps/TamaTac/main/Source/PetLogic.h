/**
 * @file PetLogic.h
 * @brief Game logic for TamaTac virtual pet
 */
#pragma once

#include "PetStats.h"
#include <Tactility/kernel/Kernel.h>

class PetLogic {
private:
    PetStats stats;

    // Internal multiplier for decay calculation: 1=0.5x, 2=1x, 4=2x
    // Set via setDecaySpeed() which converts speed enum to multiplier
    static uint8_t decayMultiplier;

public:
    PetLogic();

    // Set decay speed: 0=Slow(0.5x), 1=Normal(1x), 2=Fast(2x)
    // Maps to internal multiplier: 0->1, 1->2, 2->4
    static void setDecaySpeed(uint8_t speed);

    // Core update loop (called every 30 seconds)
    void update(uint32_t currentTimeMs);

    // Action handlers
    void performAction(PetAction action);

    // Getters
    const PetStats& getStats() const { return stats; }
    uint8_t getHunger() const { return stats.hunger; }
    uint8_t getHappiness() const { return stats.happiness; }
    uint8_t getHealth() const { return stats.health; }
    uint8_t getEnergy() const { return stats.energy; }
    uint8_t getCleanliness() const { return stats.cleanliness; }
    bool isDead() const { return stats.isDead; }
    bool isSick() const { return stats.isSick; }
    bool isAsleep() const { return stats.isAsleep; }
    LifeStage getStage() const { return stats.stage; }
    AnimState getCurrentAnimation() const;
    DayPhase getDayPhase() const;
    RandomEvent getLastEvent() const { return stats.lastEvent; }
    void clearLastEvent() { stats.lastEvent = RandomEvent::None; }

    // Random events (called during update)
    RandomEvent checkRandomEvent();

    // Mini-game result (called when pattern game completes)
    void applyPlayResult(int roundsCompleted, int maxRounds);

    // Persistence
    void saveState();
    bool loadState();
    void reset();

private:
    // State checks (called internally during update)
    void checkHealth();
    void checkEvolution();
    void applyStatDecay(uint32_t elapsedSeconds);

    // Action implementations
    void feed();
    void play();
    void giveMedicine();
    void sleep();
    void clean();
    void pet();

    // Helper functions
    uint8_t clampStat(int16_t value);
    void randomPoopCheck();
    void addPoop();
};
