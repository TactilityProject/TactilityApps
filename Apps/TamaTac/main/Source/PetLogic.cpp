/**
 * @file PetLogic.cpp
 * @brief Game logic implementation for TamaTac
 */

#include "PetLogic.h"
#include <TactilityCpp/Preferences.h>
#include <cstdlib>
#include <algorithm>

// Static member initialization (2 = normal 1x speed)
uint8_t PetLogic::decayMultiplier = 2;

void PetLogic::setDecaySpeed(uint8_t speed) {
    // speed: 0=slow, 1=normal, 2=fast
    // multiplier: 1=0.5x, 2=1x, 4=2x
    switch (speed) {
        case 0:  decayMultiplier = 1; break;  // Slow (0.5x)
        case 1:  decayMultiplier = 2; break;  // Normal (1x)
        case 2:  decayMultiplier = 4; break;  // Fast (2x)
        default: decayMultiplier = 2; break;
    }
}

PetLogic::PetLogic() {
    stats = PetStats();
    stats.lastUpdateTime = tt::kernel::getMillis();
}

void PetLogic::update(uint32_t currentTimeMs) {
    if (stats.isDead) {
        return;
    }

    // Calculate elapsed time since last update
    uint32_t elapsedMs = currentTimeMs - stats.lastUpdateTime;
    uint32_t elapsedSeconds = elapsedMs / 1000;

    // Only advance lastUpdateTime by counted milliseconds to preserve sub-second remainder
    stats.lastUpdateTime += elapsedSeconds * 1000;

    if (elapsedSeconds == 0) {
        return;  // Nothing to update yet
    }

    // Update age
    stats.ageSeconds += elapsedSeconds;
    stats.lifespan += elapsedSeconds;
    stats.ageHours = static_cast<uint16_t>(stats.ageSeconds / 3600);

    // Reset transient action animations (eating, playing, happy) back to idle
    // These are set by actions and should only persist until the next update tick
    if (stats.currentAnim == AnimState::Eating ||
        stats.currentAnim == AnimState::Playing ||
        stats.currentAnim == AnimState::Happy) {
        stats.currentAnim = AnimState::Idle;
    }

    // Apply time-based stat decay
    applyStatDecay(elapsedSeconds);

    // Check health status
    checkHealth();

    // Check evolution
    checkEvolution();

    // Random poop
    randomPoopCheck();

    // Random events
    checkRandomEvent();
}

void PetLogic::performAction(PetAction action) {
    if (stats.isDead) {
        return;
    }

    // Wake up from sleep when performing any action (except Sleep itself)
    if (action != PetAction::Sleep && stats.isAsleep) {
        stats.isAsleep = false;
        stats.currentAnim = AnimState::Idle;
    }

    switch (action) {
        case PetAction::Feed:
            feed();
            break;
        case PetAction::Play:
            play();
            break;
        case PetAction::Medicine:
            giveMedicine();
            break;
        case PetAction::Sleep:
            sleep();
            break;
        case PetAction::Clean:
            clean();
            break;
        case PetAction::Pet:
            pet();
            break;
    }
}

void PetLogic::feed() {
    uint32_t now = tt::kernel::getMillis();

    stats.hunger = clampStat(stats.hunger + ActionEffects::FEED_HUNGER_GAIN);
    stats.happiness = clampStat(stats.happiness - ActionEffects::FEED_HAPPINESS_LOSS);

    if (rand() % 100 < ActionEffects::FEED_POOP_CHANCE) {
        addPoop();
    }

    if (stats.poopCount > 0) {
        stats.cleanliness = clampStat(100 - (stats.poopCount * 30));
    }

    stats.lastFeedTime = now;
    stats.currentAnim = AnimState::Eating;
}

void PetLogic::play() {
    uint32_t now = tt::kernel::getMillis();

    stats.happiness = clampStat(stats.happiness + ActionEffects::PLAY_HAPPINESS_GAIN);
    stats.energy = clampStat(stats.energy - ActionEffects::PLAY_ENERGY_LOSS);
    stats.hunger = clampStat(stats.hunger - ActionEffects::PLAY_HUNGER_LOSS);

    stats.lastPlayTime = now;
    stats.currentAnim = AnimState::Playing;
}

void PetLogic::giveMedicine() {
    if (stats.isSick) {
        stats.isSick = false;
        stats.health = clampStat(stats.health + ActionEffects::MEDICINE_HEALTH_GAIN);
        stats.currentAnim = AnimState::Idle;
    } else {
        stats.health = clampStat(stats.health + ActionEffects::MEDICINE_HEALTH_BOOST);
    }
}

void PetLogic::sleep() {
    stats.energy = clampStat(stats.energy + ActionEffects::SLEEP_ENERGY_GAIN);
    stats.happiness = clampStat(stats.happiness - ActionEffects::SLEEP_HAPPINESS_LOSS);

    stats.isAsleep = true;
    stats.currentAnim = AnimState::Sleeping;
}

void PetLogic::clean() {
    if (stats.poopCount > 0) {
        stats.poopCount = 0;
        stats.cleanliness = PetThresholds::STAT_MAX;
        stats.happiness = clampStat(stats.happiness + 10);
    }
}

void PetLogic::pet() {
    stats.happiness = clampStat(stats.happiness + ActionEffects::PET_HAPPINESS_GAIN);
    stats.currentAnim = AnimState::Happy;
}

void PetLogic::applyPlayResult(int roundsCompleted, int maxRounds) {
    if (stats.isDead) return;

    if (stats.isAsleep) {
        stats.isAsleep = false;
        stats.currentAnim = AnimState::Idle;
    }

    if (maxRounds > 0 && roundsCompleted >= maxRounds) {
        // Full win: generous reward
        stats.happiness = clampStat(stats.happiness + 30);
        stats.energy = clampStat(stats.energy - 10);
        stats.hunger = clampStat(stats.hunger - 5);
    } else if (roundsCompleted > 0) {
        // Partial: scaled reward
        stats.happiness = clampStat(stats.happiness + 10 + roundsCompleted * 5);
        stats.energy = clampStat(stats.energy - 5);
    } else {
        // Lost immediately: token reward
        stats.happiness = clampStat(stats.happiness + 5);
        stats.energy = clampStat(stats.energy - 5);
    }

    stats.currentAnim = AnimState::Playing;
    stats.lastPlayTime = tt::kernel::getMillis();
}

void PetLogic::applyStatDecay(uint32_t elapsedSeconds) {
    uint32_t ticks = elapsedSeconds / DecayRates::TICK_INTERVAL_SEC;
    if (ticks == 0) return;

    // Cap ticks to prevent extreme decay after very long absence
    if (ticks > 100) ticks = 100;

    // Apply decay multiplier (1=slow 0.5x, 2=normal 1x, 4=fast 2x)
    // We multiply ticks by decayMultiplier/2 to get effective ticks
    uint32_t effectiveTicks = (ticks * decayMultiplier) / 2;
    if (effectiveTicks == 0 && ticks > 0) effectiveTicks = 1;  // At least 1 tick if time passed

    for (uint32_t i = 0; i < effectiveTicks; i++) {
        stats.hunger = clampStat(stats.hunger - DecayRates::HUNGER_DECAY);
        stats.happiness = clampStat(stats.happiness - DecayRates::HAPPINESS_DECAY);
        stats.energy = clampStat(stats.energy - DecayRates::ENERGY_DECAY);

        if (stats.hunger < PetThresholds::HUNGRY_THRESHOLD ||
            stats.happiness < PetThresholds::UNHAPPY_THRESHOLD) {
            stats.health = clampStat(stats.health - DecayRates::HEALTH_DECAY_NEGLECT);
        }
    }

    // Apply personality and day/night modifiers (per tick)
    DayPhase phase = getDayPhase();
    for (uint32_t i = 0; i < effectiveTicks; i++) {
        // Personality modifiers
        switch (stats.personality) {
            case Personality::Energetic:
                stats.energy = clampStat(stats.energy + 1);
                stats.hunger = clampStat(stats.hunger - 1);
                break;
            case Personality::Lazy:
                stats.energy = clampStat(stats.energy - 1);
                stats.happiness = clampStat(stats.happiness + 1);
                break;
            case Personality::Glutton:
                stats.hunger = clampStat(stats.hunger - 1);
                stats.cleanliness = clampStat(stats.cleanliness - 1);
                break;
            case Personality::Cheerful:
                stats.happiness = clampStat(stats.happiness + 1);
                stats.energy = clampStat(stats.energy - 1);
                break;
            case Personality::Hardy:
                stats.health = clampStat(stats.health + 1);
                stats.hunger = clampStat(stats.hunger - 1);
                break;
        }
        // Day/night modifiers
        if (phase == DayPhase::Day) {
            stats.energy = clampStat(stats.energy - 1);  // More active during day
        } else {
            stats.hunger = clampStat(stats.hunger - 1);  // Metabolism at night
            // Auto-sleep if energy critically low at night
            if (!stats.isAsleep && stats.energy < PetThresholds::TIRED_THRESHOLD) {
                stats.isAsleep = true;
                stats.currentAnim = AnimState::Sleeping;
            }
        }
    }

    if (stats.poopCount > 0) {
        stats.cleanliness = clampStat(100 - (stats.poopCount * 30));
    }
}

void PetLogic::checkHealth() {
    if (stats.health < PetThresholds::SICK_THRESHOLD && !stats.isDead) {
        stats.isSick = true;
    }

    if (stats.health <= 0) {
        stats.isDead = true;
        stats.isSick = false;
        stats.stage = LifeStage::Ghost;
        stats.currentAnim = AnimState::Dead;
    }
}

void PetLogic::checkEvolution() {
    if (stats.isDead) {
        return;
    }

    LifeStage newStage = stats.stage;

    if (stats.ageHours < 1) {
        newStage = LifeStage::Egg;
    } else if (stats.ageHours < 8) {
        newStage = LifeStage::Baby;
    } else if (stats.ageHours < 24) {
        newStage = LifeStage::Teen;
    } else if (stats.ageHours < 72) {
        newStage = LifeStage::Adult;
    } else {
        newStage = LifeStage::Elder;
    }

    if (newStage != stats.stage) {
        stats.stage = newStage;
    }
}

DayPhase PetLogic::getDayPhase() const {
    return (stats.ageSeconds % 3600 < 1800) ? DayPhase::Day : DayPhase::Night;
}

AnimState PetLogic::getCurrentAnimation() const {
    if (stats.isDead) {
        return AnimState::Dead;
    }

    if (stats.isAsleep) {
        return AnimState::Sleeping;
    }

    if (stats.isSick) {
        return AnimState::Sick;
    }

    if (stats.happiness < PetThresholds::UNHAPPY_THRESHOLD) {
        return AnimState::Sad;
    }

    return stats.currentAnim;
}


void PetLogic::randomPoopCheck() {
    if (rand() % 1000 < 10) {
        addPoop();
    }
}

void PetLogic::addPoop() {
    if (stats.poopCount < 3) {
        stats.poopCount++;
    }
}

RandomEvent PetLogic::checkRandomEvent() {
    if (stats.isDead || stats.isAsleep) return RandomEvent::None;
    if (rand() % 100 >= 12) return RandomEvent::None;  // 12% chance per tick

    // Build eligible events based on current state
    RandomEvent eligible[6];
    int count = 0;

    if (stats.hunger < 85) eligible[count++] = RandomEvent::FoundTreat;
    if (stats.happiness < 85) eligible[count++] = RandomEvent::MadeFriend;
    if (!stats.isSick && stats.health > 30) eligible[count++] = RandomEvent::CaughtCold;
    if (stats.cleanliness > 30) eligible[count++] = RandomEvent::GotMuddy;
    if (stats.energy < 85) eligible[count++] = RandomEvent::HadNap;
    if (getDayPhase() == DayPhase::Day && stats.happiness < 90) eligible[count++] = RandomEvent::SunnyDay;

    if (count == 0) return RandomEvent::None;

    RandomEvent event = eligible[rand() % count];

    // Apply effects
    switch (event) {
        case RandomEvent::FoundTreat:
            stats.hunger = clampStat(stats.hunger + 15);
            break;
        case RandomEvent::MadeFriend:
            stats.happiness = clampStat(stats.happiness + 15);
            break;
        case RandomEvent::CaughtCold:
            stats.isSick = true;
            stats.health = clampStat(stats.health - 10);
            break;
        case RandomEvent::GotMuddy:
            stats.cleanliness = clampStat(stats.cleanliness - 20);
            break;
        case RandomEvent::HadNap:
            stats.energy = clampStat(stats.energy + 15);
            break;
        case RandomEvent::SunnyDay:
            stats.happiness = clampStat(stats.happiness + 10);
            break;
        default:
            break;
    }

    stats.lastEvent = event;
    return event;
}

uint8_t PetLogic::clampStat(int16_t value) {
    if (value < PetThresholds::STAT_MIN) {
        return PetThresholds::STAT_MIN;
    }
    if (value > PetThresholds::STAT_MAX) {
        return PetThresholds::STAT_MAX;
    }
    return static_cast<uint8_t>(value);
}

void PetLogic::saveState() {
    Preferences prefs("TamaTac");

    prefs.putInt32("hunger", stats.hunger);
    prefs.putInt32("happiness", stats.happiness);
    prefs.putInt32("health", stats.health);
    prefs.putInt32("energy", stats.energy);

    prefs.putInt32("cleanliness", stats.cleanliness);
    prefs.putInt32("poopCount", stats.poopCount);

    prefs.putInt32("ageSeconds", stats.ageSeconds);
    prefs.putInt32("ageHours", stats.ageHours);
    prefs.putInt32("lifespan", stats.lifespan);

    prefs.putBool("isSick", stats.isSick);
    prefs.putBool("isAsleep", stats.isAsleep);
    prefs.putBool("isDead", stats.isDead);

    prefs.putInt32("stage", static_cast<int32_t>(stats.stage));
    prefs.putInt32("currentAnim", static_cast<int32_t>(stats.currentAnim));
    prefs.putInt32("personality", static_cast<int32_t>(stats.personality));

    // Note: uint32_t millis stored as int32_t (Preferences API limitation).
    // Two's complement round-trips correctly; no unsigned API available.
    prefs.putInt32("lastSaveTime", static_cast<int32_t>(tt::kernel::getMillis()));
}

bool PetLogic::loadState() {
    Preferences prefs("TamaTac");

    // Check if save data exists
    int32_t savedHunger = prefs.getInt32("hunger", -1);
    if (savedHunger == -1) {
        return false;
    }

    // Clamp loaded values to valid ranges (guard against corrupted preferences)
    stats.hunger = clampStat(savedHunger);
    stats.happiness = clampStat(prefs.getInt32("happiness", 70));
    stats.health = clampStat(prefs.getInt32("health", 100));
    stats.energy = clampStat(prefs.getInt32("energy", 90));

    stats.cleanliness = clampStat(prefs.getInt32("cleanliness", 100));
    int32_t loadedPoopCount = prefs.getInt32("poopCount", 0);
    stats.poopCount = (loadedPoopCount < 0) ? 0 : (loadedPoopCount > 3) ? 3 : static_cast<uint8_t>(loadedPoopCount);

    // uint32_t fields stored as int32_t (Preferences API limitation); cast back explicitly
    stats.ageSeconds = static_cast<uint32_t>(prefs.getInt32("ageSeconds", 0));
    stats.ageHours = static_cast<uint16_t>(prefs.getInt32("ageHours", 0));
    stats.lifespan = static_cast<uint32_t>(prefs.getInt32("lifespan", 0));

    stats.isSick = prefs.getBool("isSick", false);
    stats.isAsleep = prefs.getBool("isAsleep", false);
    stats.isDead = prefs.getBool("isDead", false);

    int32_t loadedStage = prefs.getInt32("stage", static_cast<int32_t>(LifeStage::Egg));
    if (loadedStage < 0 || loadedStage > static_cast<int32_t>(LifeStage::Ghost)) {
        loadedStage = static_cast<int32_t>(LifeStage::Egg);
    }
    stats.stage = static_cast<LifeStage>(loadedStage);

    int32_t loadedAnim = prefs.getInt32("currentAnim", static_cast<int32_t>(AnimState::Idle));
    if (loadedAnim < 0 || loadedAnim > static_cast<int32_t>(AnimState::Dead)) {
        loadedAnim = static_cast<int32_t>(AnimState::Idle);
    }
    stats.currentAnim = static_cast<AnimState>(loadedAnim);

    int32_t loadedPersonality = prefs.getInt32("personality", 0);
    if (loadedPersonality < 0 || loadedPersonality > static_cast<int32_t>(Personality::Hardy)) {
        loadedPersonality = 0;
    }
    stats.personality = static_cast<Personality>(loadedPersonality);

    // Handle time elapsed since last save
    uint32_t lastSaveTime = prefs.getInt32("lastSaveTime", 0);
    uint32_t currentTime = tt::kernel::getMillis();

    if (lastSaveTime > 0 && currentTime > lastSaveTime) {
        uint32_t elapsedMs = currentTime - lastSaveTime;
        uint32_t elapsedSeconds = elapsedMs / 1000;

        stats.ageSeconds += elapsedSeconds;
        stats.lifespan += elapsedSeconds;
        stats.ageHours = static_cast<uint16_t>(stats.ageSeconds / 3600);

        // Apply time-based decay for time away
        applyStatDecay(elapsedSeconds);

        checkHealth();
        checkEvolution();
    }

    stats.lastUpdateTime = currentTime;

    return true;
}

void PetLogic::reset() {
    stats = PetStats();
    stats.personality = static_cast<Personality>(rand() % 5);
    stats.lastUpdateTime = tt::kernel::getMillis();
}
