#pragma once

#include "Track.h"
#include "Vehicle.h"

#include <string>
#include <vector>

namespace hd {

/**
 * One tick of control for one racer: what a player (local or remote) or the AI wants to do.
 * Small and plain, so it can be sent over a network as-is (see net/Protocol.h).
 */
struct RacerInput {
    float steer = 0.0f; // -1 (left) .. 1 (right)
    bool throttle = false;
    bool brake = false;
    bool boost = false;
};

enum class RacerMode : uint8_t {
    Driving,
    Falling, // fell off the track, dropping into the void
    Respawning, // waiting to be put back on the track
};

/** Everything the simulation knows about one racer. */
struct Racer {
    // Identity (fixed for the race)
    int id = 0;
    std::string name;
    int vehicle = 0; // index into the race's vehicle list
    bool human = false; // driven by a person (local or remote), not the AI

    // Motion
    Vec2 position;
    Vec2 velocity;
    float heading = 0.0f; // radians, see headingToDirection()
    float height = 0.0f; // above the zero plane
    float verticalSpeed = 0.0f;
    bool airborne = false;
    float climbRate = 0.0f; // vertical speed while following the road (ramps), kept on take-off
    float boostEnergy = 1.0f; // 0..1
    bool boosting = false;
    RacerMode mode = RacerMode::Driving;
    float modeTime = 0.0f; // seconds in the current mode
    float ghostTime = 0.0f; // after a respawn: no collisions for a while
    float wallHitTime = 0.0f; // since the last barrier hit, for effects
    float wrongWayTime = 0.0f;

    // Track and race progress
    TrackLocation location;
    int lapsCompleted = -1; // -1 while still behind the start line on the grid
    bool passedHalfway = false;
    float raceDistance = 0.0f; // laps * length + distance; what positions are ranked by
    float lapStartTime = 0.0f;
    float lastLapTime = 0.0f;
    float bestLapTime = 0.0f;
    bool finished = false;
    float finishTime = 0.0f;
    int place = 1;
    int falls = 0;

    RacerInput input; // the last input applied

    float speed() const { return length(velocity); }
};

enum class RacePhase : uint8_t {
    Countdown,
    Racing,
    Finished, // all racers are done (or time ran out after the local players finished)
};

/**
 * The race simulation. Deterministic: given the same track, vehicles, racers and per-tick inputs
 * it produces the same race, which is what a lockstep network mode needs. It knows nothing about
 * rendering, input devices or who controls a racer.
 */
class RaceSim {
public:
    static constexpr float TICK = 1.0f / 60.0f;
    static constexpr float COUNTDOWN = 3.0f;

    /** Prepares a race. Racers are added afterwards with addRacer(). */
    void setup(const Track* track, const std::vector<VehicleSpec>* vehicles, int laps);

    /** @return the new racer's id (its index) */
    int addRacer(const std::string& name, int vehicle, bool human);

    /** Puts the racers on the starting grid (in the order they were added, front first). */
    void start();

    /**
     * Advances the race by one tick.
     * @param inputs one entry per racer (by id)
     */
    void step(const std::vector<RacerInput>& inputs);

    RacePhase phase() const { return phase_; }
    /** Seconds since the start signal (negative during the countdown). */
    float raceTime() const { return time_; }
    uint32_t tick() const { return tick_; }
    int laps() const { return laps_; }
    const Track& track() const { return *track_; }
    const VehicleSpec& vehicle(const Racer& racer) const { return (*vehicles_)[racer.vehicle]; }
    const std::vector<Racer>& racers() const { return racers_; }
    const Racer& racer(int id) const { return racers_[id]; }
    int racerCount() const { return (int)racers_.size(); }

    /** Ends the race early for everyone still racing, ranking them by distance. */
    void finishRemaining();

private:
    const Track* track_ = nullptr;
    const std::vector<VehicleSpec>* vehicles_ = nullptr;
    std::vector<Racer> racers_;
    int laps_ = 3;
    RacePhase phase_ = RacePhase::Countdown;
    float time_ = -COUNTDOWN;
    uint32_t tick_ = 0;
    float allHumansFinishedTime = -1.0f;

    void stepRacer(Racer& racer, const RacerInput& input);
    void updateProgress(Racer& racer, float previousDistance);
    void respawn(Racer& racer);
    void collideRacers();
    void rank();
};

} // namespace hd
