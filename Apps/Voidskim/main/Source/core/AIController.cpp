#include "Controller.h"

namespace hd {

const char* difficultyName(Difficulty difficulty) {
    switch (difficulty) {
        case Difficulty::Easy: return "EASY";
        case Difficulty::Normal: return "NORMAL";
        case Difficulty::Hard: return "HARD";
    }
    return "?";
}

AIController::AIController(Difficulty difficulty, uint32_t seed) : random(seed) {
    switch (difficulty) {
        case Difficulty::Easy:
            speedSkill = 0.8f; cornerSkill = 0.75f; steerGain = 2.0f; steerNoise = 0.25f; boostEagerness = 0.15f;
            break;
        case Difficulty::Normal:
            speedSkill = 0.91f; cornerSkill = 0.88f; steerGain = 2.6f; steerNoise = 0.12f; boostEagerness = 0.5f;
            break;
        case Difficulty::Hard:
            speedSkill = 1.0f; cornerSkill = 1.0f; steerGain = 3.2f; steerNoise = 0.04f; boostEagerness = 0.9f;
            break;
    }
    // A little variety between opponents of the same difficulty
    speedSkill *= random.range(0.97f, 1.02f);
    laneTarget = random.range(-0.4f, 0.4f);
    lane = laneTarget;
}

RacerInput AIController::control(const RaceSim& race, int racerId) {
    RacerInput input;
    const Racer& racer = race.racer(racerId);
    if (racer.mode != RacerMode::Driving) {
        return input;
    }
    const Track& track = race.track();
    const VehicleSpec& spec = race.vehicle(racer);
    const float dt = RaceSim::TICK;
    const float speed = racer.speed();

    // Wander between lanes now and then, so the pack spreads out
    laneTimer -= dt;
    if (laneTimer <= 0.0f) {
        laneTimer = random.range(2.0f, 5.0f);
        laneTarget = random.range(-0.5f, 0.5f);
    }
    noiseTimer -= dt;
    if (noiseTimer <= 0.0f) {
        noiseTimer = random.range(0.3f, 0.8f);
        noise = random.range(-steerNoise, steerNoise);
    }

    // Look at the road ahead: hazards keep us centred, open edges keep us away from them
    const float distance = racer.location.distance;
    const float lookahead = 8.0f + speed * 0.45f;
    bool jumpAhead = false;
    bool openEdgeAhead = false;
    const int scanSamples = (int)((speed * 1.6f + 30.0f) / 4.0f);
    for (int i = 0; i < scanSamples; i++) {
        const TrackSample& s = track.sample(racer.location.segment + i);
        jumpAhead = jumpAhead || s.gap || s.ramp;
        openEdgeAhead = openEdgeAhead || !s.wallLeft || !s.wallRight;
    }
    float wantedLane = laneTarget;
    if (jumpAhead) wantedLane = 0.0f;
    else if (openEdgeAhead) wantedLane = clampf(laneTarget, -0.2f, 0.2f);
    lane = approach(lane, wantedLane, 0.5f * dt);

    // Steer towards a point on our lane ahead
    Vec2 aimPoint;
    Vec2 aimDirection;
    track.pointAt(distance + lookahead, aimPoint, aimDirection);
    const TrackLocation aimLocation = track.locate(aimPoint, racer.location.segment);
    aimPoint += rightOf(aimDirection) * (lane * aimLocation.halfWidth);
    const float wanted = directionToHeading(aimPoint - racer.position);
    float error = wrapAngle(wanted - racer.heading);
    if (racer.wrongWayTime > 0.5f) {
        error = wrapAngle(directionToHeading(racer.location.direction) - racer.heading);
    }
    input.steer = clampf(error * steerGain + noise, -1.0f, 1.0f);

    // Speed: as fast as the sharpest corner ahead allows
    const float curvature = std::fabs(track.curvatureAhead(distance + 4.0f, speed * 1.2f + 20.0f));
    float targetSpeed = spec.topSpeed * speedSkill;
    if (curvature > 1e-4f) {
        const float cornerSpeed = spec.turnRate * 0.8f / curvature * cornerSkill;
        targetSpeed = std::fmin(targetSpeed, std::fmax(cornerSpeed, 15.0f));
    }
    if (jumpAhead) {
        targetSpeed = spec.topSpeed; // jumps need speed
    }
    input.throttle = speed < targetSpeed;
    input.brake = !jumpAhead && speed > targetSpeed * 1.1f;

    // Boost on straights and before jumps, if there's enough in the tank
    const bool straight = curvature < 0.004f;
    if (racer.boostEnergy > 0.3f && input.throttle && (straight || jumpAhead)) {
        input.boost = random.unit() < boostEagerness * 0.2f || racer.boosting || jumpAhead;
    }
    return input;
}

} // namespace hd
