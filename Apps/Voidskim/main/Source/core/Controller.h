#pragma once

#include "Race.h"

namespace hd {

/**
 * Decides a racer's input every tick. The race doesn't care who's behind it:
 * - LocalController: a person on this device (InputState from the platform)
 * - AIController: the computer
 * - RemoteController: a person on another device, fed by a NetSession (see net/)
 */
class Controller {
public:
    virtual ~Controller() = default;
    virtual RacerInput control(const RaceSim& race, int racerId) = 0;
};

/** A person on this device. The game sets the input every tick from the input devices. */
class LocalController final : public Controller {
public:
    void set(const RacerInput& input) { input_ = input; }
    RacerInput control(const RaceSim&, int) override { return input_; }

private:
    RacerInput input_;
};

enum class Difficulty : uint8_t { Easy, Normal, Hard };

const char* difficultyName(Difficulty difficulty);

/** The computer: follows a racing line, slows for corners, boosts on straights and for jumps. */
class AIController final : public Controller {
public:
    AIController(Difficulty difficulty, uint32_t seed);
    RacerInput control(const RaceSim& race, int racerId) override;

private:
    // Skill
    float speedSkill = 0.9f; // fraction of the craft's real limits it drives at
    float cornerSkill = 0.9f; // how much cornering speed it dares
    float steerGain = 2.5f;
    float steerNoise = 0.1f; // sloppiness
    float boostEagerness = 0.5f; // 0..1

    Random random;
    float laneTarget = 0.0f; // preferred offset from the centreline, as a fraction of the half width
    float lane = 0.0f;
    float laneTimer = 0.0f;
    float noise = 0.0f;
    float noiseTimer = 0.0f;
};

} // namespace hd
