/**
 * @file PetStats.h
 * @brief Data structures for TamaTac pet state
 */
#pragma once

#include <cstdint>

// Pet actions
enum class PetAction {
    Feed,
    Play,
    Medicine,
    Sleep,
    Clean,
    Pet       // Tap-to-pet: small happiness boost
};

// Life stages (evolution)
enum class LifeStage {
    Egg,      // 0-1 hour
    Baby,     // 1-8 hours
    Teen,     // 8-24 hours
    Adult,    // 24-72 hours
    Elder,    // 72+ hours
    Ghost     // Dead
};

// Pet personality (affects stat decay rates)
enum class Personality : uint8_t {
    Energetic = 0,  // -1 energy decay, +1 hunger decay
    Lazy = 1,       // +1 energy decay, -1 happiness decay
    Glutton = 2,    // +1 hunger decay, +1 cleanliness decay
    Cheerful = 3,   // -1 happiness decay, +1 energy decay
    Hardy = 4       // -1 health_neglect decay, +1 hunger decay
};

// Day/night cycle (derived from ageSeconds, not stored)
enum class DayPhase : uint8_t { Day = 0, Night = 1 };

// Random events (transient, not saved)
enum class RandomEvent : uint8_t {
    None = 0,
    FoundTreat,    // +15 hunger
    MadeFriend,    // +15 happiness
    CaughtCold,    // becomes sick, -10 health
    GotMuddy,      // -20 cleanliness
    HadNap,        // +15 energy
    SunnyDay       // +10 happiness (day only)
};

// Animation states (for sprites)
enum class AnimState {
    Idle,
    Happy,
    Sad,
    Sleeping,
    Eating,
    Playing,
    Sick,
    Dead
};

// Pet statistics and state
struct PetStats {
    // Primary stats (0-100 range)
    uint8_t hunger;      // 100 = full, 0 = starving
    uint8_t happiness;   // 100 = very happy, 0 = depressed
    uint8_t health;      // 100 = healthy, 0 = dead
    uint8_t energy;      // 100 = energized, 0 = exhausted

    // Secondary stats
    uint8_t cleanliness; // 100 = clean, 0 = filthy
    uint8_t poopCount;   // Number of poops that need cleaning

    // Age tracking
    uint32_t ageSeconds; // Total age in seconds
    uint16_t ageHours;   // Age in hours (for evolution) - supports up to ~7.5 years

    // Longevity (for high scores)
    uint32_t lifespan;   // Total seconds alive (resets on death)

    // State flags
    bool isSick;         // True when health < 20
    bool isAsleep;       // True during sleep action
    bool isDead;         // True when health reaches 0

    // Current state
    LifeStage stage;
    AnimState currentAnim;
    Personality personality;
    RandomEvent lastEvent;    // Transient — not saved/loaded

    // Timestamps
    uint32_t lastUpdateTime;  // Last stat update (millis)
    uint32_t lastFeedTime;    // Last time fed (millis)
    uint32_t lastPlayTime;    // Last time played (millis)

    // Initialize with default values
    PetStats() :
        hunger(80),
        happiness(70),
        health(100),
        energy(90),
        cleanliness(100),
        poopCount(0),
        ageSeconds(0),
        ageHours(0),
        lifespan(0),
        isSick(false),
        isAsleep(false),
        isDead(false),
        stage(LifeStage::Egg),
        currentAnim(AnimState::Idle),
        personality(Personality::Energetic),
        lastEvent(RandomEvent::None),
        lastUpdateTime(0),
        lastFeedTime(0),
        lastPlayTime(0)
    {}
};

// Enum-to-string helpers (shared across views)
inline const char* lifeStageToString(LifeStage stage) {
    switch (stage) {
        case LifeStage::Egg:   return "Egg";
        case LifeStage::Baby:  return "Baby";
        case LifeStage::Teen:  return "Teen";
        case LifeStage::Adult: return "Adult";
        case LifeStage::Elder: return "Elder";
        case LifeStage::Ghost: return "Ghost";
        default:               return "Unknown";
    }
}

inline const char* personalityToString(Personality p) {
    switch (p) {
        case Personality::Energetic: return "Energetic";
        case Personality::Lazy:      return "Lazy";
        case Personality::Glutton:   return "Glutton";
        case Personality::Cheerful:  return "Cheerful";
        case Personality::Hardy:     return "Hardy";
        default:                     return "Unknown";
    }
}

// Stat thresholds
namespace PetThresholds {
    constexpr uint8_t STAT_MAX = 100;
    constexpr uint8_t STAT_MIN = 0;

    constexpr uint8_t HUNGRY_THRESHOLD = 30;
    constexpr uint8_t UNHAPPY_THRESHOLD = 30;
    constexpr uint8_t SICK_THRESHOLD = 20;
    constexpr uint8_t TIRED_THRESHOLD = 20;
    constexpr uint8_t DIRTY_THRESHOLD = 30;
}

// Decay rates (per 30-second tick)
namespace DecayRates {
    constexpr uint8_t HUNGER_DECAY = 2;      // Hungry every ~15 minutes
    constexpr uint8_t HAPPINESS_DECAY = 1;   // Bored every ~30 minutes
    constexpr uint8_t ENERGY_DECAY = 2;      // Tired every ~15 minutes
    constexpr uint8_t HEALTH_DECAY_NEGLECT = 1;  // When hungry or unhappy

    constexpr uint32_t TICK_INTERVAL_SEC = 30;  // Decay rates are calibrated for this interval
}

// Action effects
namespace ActionEffects {
    constexpr uint8_t FEED_HUNGER_GAIN = 30;
    constexpr uint8_t FEED_HAPPINESS_LOSS = 5;  // Boring to just eat
    constexpr uint8_t FEED_POOP_CHANCE = 40;    // 40% chance to poop

    constexpr uint8_t PLAY_HAPPINESS_GAIN = 25;
    constexpr uint8_t PLAY_ENERGY_LOSS = 10;
    constexpr uint8_t PLAY_HUNGER_LOSS = 5;

    constexpr uint8_t MEDICINE_HEALTH_GAIN = 30;
    constexpr uint8_t MEDICINE_HEALTH_BOOST = 5;  // When not sick

    constexpr uint8_t SLEEP_ENERGY_GAIN = 50;
    constexpr uint8_t SLEEP_HAPPINESS_LOSS = 10; // Boring to sleep
    constexpr uint32_t SLEEP_DURATION_MS = 5000; // 5 seconds sleep animation

    constexpr uint8_t PET_HAPPINESS_GAIN = 5;
}
