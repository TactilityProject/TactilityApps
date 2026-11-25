#include "Race.h"

#include <algorithm>

namespace hd {

// Tuning. Distances in metres, times in seconds.
constexpr float GRAVITY = 30.0f;
constexpr float CRAFT_RADIUS = 1.6f; // racer-to-racer collisions
constexpr float CRAFT_HALF_WIDTH = 1.3f; // barrier collisions
constexpr float WALL_HEIGHT = 1.6f; // a craft higher above the road than this clears the barrier
constexpr float WALL_BOUNCE = 0.35f;
constexpr float WALL_SPEED_LOSS = 0.9f;
constexpr float DRAG = 0.12f; // per second, always
constexpr float COAST_DRAG = 0.35f; // per second, extra when not on the throttle
constexpr float REVERSE_SPEED = 10.0f;
constexpr float FALL_LIMIT = -5.0f; // below this a craft that left the track is lost
constexpr float FALL_DURATION = 1.0f; // watching it drop
constexpr float RESPAWN_DURATION = 1.6f; // off the track: together with the fall, the time penalty
constexpr float GHOST_DURATION = 2.0f;
constexpr float GRID_SPACING = 9.0f;
constexpr float FINISH_GRACE = 20.0f; // after the humans finish, how long the rest get

void RaceSim::setup(const Track* track, const std::vector<VehicleSpec>* vehicles, int laps) {
    track_ = track;
    vehicles_ = vehicles;
    laps_ = laps;
    racers_.clear();
    phase_ = RacePhase::Countdown;
    time_ = -COUNTDOWN;
    tick_ = 0;
    allHumansFinishedTime = -1.0f;
}

int RaceSim::addRacer(const std::string& name, int vehicle, bool human) {
    Racer racer;
    racer.id = (int)racers_.size();
    racer.name = name;
    racer.vehicle = vehicle;
    racer.human = human;
    racers_.push_back(racer);
    return racer.id;
}

void RaceSim::start() {
    // Two columns, staggered, behind the start line
    for (auto& racer : racers_) {
        const int row = racer.id / 2;
        const float side = (racer.id % 2 == 0) ? -0.4f : 0.4f; // fraction of the half width
        const float behind = GRID_SPACING * (float)(row + 1) + (racer.id % 2) * GRID_SPACING * 0.5f;
        Vec2 centre;
        Vec2 direction;
        track_->pointAt(track_->length() - behind, centre, direction);
        const TrackLocation location = track_->locate(centre, -1);
        racer.position = centre + rightOf(direction) * (side * location.halfWidth);
        racer.heading = directionToHeading(direction);
        racer.velocity = {};
        racer.height = 0.0f;
        racer.location = track_->locate(racer.position, -1);
        racer.lapsCompleted = -1;
        racer.raceDistance = racer.location.distance - track_->length(); // lap -1
    }
    rank();
}

void RaceSim::step(const std::vector<RacerInput>& inputs) {
    tick_++;
    time_ += TICK;
    if (phase_ == RacePhase::Countdown && time_ >= 0.0f) {
        phase_ = RacePhase::Racing;
        for (auto& racer : racers_) {
            racer.lapStartTime = 0.0f;
        }
    }

    for (auto& racer : racers_) {
        RacerInput input = racer.id < (int)inputs.size() ? inputs[racer.id] : RacerInput();
        if (phase_ == RacePhase::Countdown) {
            input = RacerInput(); // held on the grid
        }
        stepRacer(racer, input);
    }
    collideRacers();
    rank();

    if (phase_ == RacePhase::Racing) {
        bool allFinished = true;
        bool anyHuman = false;
        bool humansFinished = true;
        for (const auto& racer : racers_) {
            allFinished = allFinished && racer.finished;
            if (racer.human) {
                anyHuman = true;
                humansFinished = humansFinished && racer.finished;
            }
        }
        humansFinished = humansFinished && anyHuman;
        if (humansFinished && allHumansFinishedTime < 0.0f) {
            allHumansFinishedTime = time_;
        }
        if (allFinished || (allHumansFinishedTime >= 0.0f && time_ - allHumansFinishedTime > FINISH_GRACE)) {
            finishRemaining();
        }
    }
}

void RaceSim::finishRemaining() {
    for (auto& racer : racers_) {
        if (!racer.finished) {
            racer.finished = true;
            racer.finishTime = 1e9f; // ranked behind everyone who finished, by distance
        }
    }
    rank();
    phase_ = RacePhase::Finished;
}

void RaceSim::stepRacer(Racer& racer, const RacerInput& input) {
    const float dt = TICK;
    const VehicleSpec& spec = vehicle(racer);
    racer.input = input;
    racer.modeTime += dt;
    racer.ghostTime = std::max(0.0f, racer.ghostTime - dt);
    racer.wallHitTime += dt;

    if (racer.mode == RacerMode::Respawning) {
        if (racer.modeTime >= RESPAWN_DURATION) {
            respawn(racer);
        }
        return;
    }

    const bool driving = racer.mode == RacerMode::Driving;
    const Vec2 forward = headingToDirection(racer.heading);
    const Vec2 right = rightOf(forward);
    float forwardSpeed = dot(racer.velocity, forward);
    float sideSpeed = dot(racer.velocity, right);

    // Boost: a tank that drains while boosting and recharges otherwise
    racer.boosting = driving && input.boost && input.throttle && racer.boostEnergy > 0.0f && forwardSpeed > 5.0f;
    if (racer.boosting) {
        racer.boostEnergy = std::max(0.0f, racer.boostEnergy - dt / spec.boostTime);
    } else {
        racer.boostEnergy = std::min(1.0f, racer.boostEnergy + spec.boostRecharge * dt);
    }

    // Steering: weaker when nearly stopped and at very high speed, much weaker in the air
    if (driving) {
        const float speedFactor = clampf(std::fabs(forwardSpeed) / 14.0f, 0.15f, 1.0f) *
            (1.0f - 0.25f * clampf(forwardSpeed / (spec.topSpeed + spec.boostSpeed), 0.0f, 1.0f));
        const float control = racer.airborne ? spec.airControl : 1.0f;
        const float direction = forwardSpeed < -0.5f ? -1.0f : 1.0f; // reversing steers like a car
        const float turn = clampf(input.steer, -1.0f, 1.0f) * spec.turnRate * speedFactor * control * direction * dt;
        racer.heading = wrapAngle(racer.heading + turn);
        if (!racer.airborne) {
            // Most of the momentum follows the nose; grip removes the rest of the drift below
            const float carried = turn * 0.88f;
            const float c = std::cos(carried);
            const float s = std::sin(carried);
            const Vec2 v = racer.velocity;
            racer.velocity = { v.x * c + v.z * s, -v.x * s + v.z * c };
        }
    }

    const Vec2 newForward = headingToDirection(racer.heading);
    const Vec2 newRight = rightOf(newForward);
    forwardSpeed = dot(racer.velocity, newForward);
    sideSpeed = dot(racer.velocity, newRight);

    if (driving && !racer.airborne) {
        const float maxSpeed = spec.topSpeed + (racer.boosting ? spec.boostSpeed : 0.0f);
        if (input.throttle && forwardSpeed < maxSpeed) {
            const float accel = spec.acceleration * (racer.boosting ? 1.8f : 1.0f);
            // Pulls less as it approaches top speed
            forwardSpeed += accel * dt * clampf(1.15f - forwardSpeed / maxSpeed, 0.1f, 1.0f);
        }
        if (input.brake) {
            if (forwardSpeed > 0.0f) {
                forwardSpeed = std::max(0.0f, forwardSpeed - spec.braking * dt);
            } else if (forwardSpeed > -REVERSE_SPEED) {
                forwardSpeed -= spec.acceleration * 0.5f * dt;
            }
        }
        if (forwardSpeed > maxSpeed) {
            forwardSpeed = approach(forwardSpeed, maxSpeed, 25.0f * dt); // easing off after a boost
        }
        forwardSpeed -= forwardSpeed * (DRAG + (input.throttle ? 0.0f : COAST_DRAG)) * dt;
        sideSpeed *= std::exp(-spec.grip * dt);
    } else {
        forwardSpeed -= forwardSpeed * DRAG * 0.5f * dt;
    }
    racer.velocity = newForward * forwardSpeed + newRight * sideSpeed;

    // Move
    const float previousDistance = racer.location.distance;
    racer.position += racer.velocity * dt;
    racer.location = track_->locate(racer.position, racer.location.segment);
    const TrackLocation& loc = racer.location;

    // Height: follow the road, or fly
    if (!racer.airborne) {
        if (!loc.hasSurface) {
            racer.airborne = true; // off the edge or over a gap
            racer.verticalSpeed = racer.climbRate;
        } else if (racer.height - loc.surfaceHeight > 0.35f + racer.speed() * dt * 0.5f) {
            // The road dropped away faster than a slope does: off the top of a ramp
            racer.airborne = true;
            racer.verticalSpeed = racer.climbRate;
        } else {
            racer.climbRate = (loc.surfaceHeight - racer.height) / dt;
            racer.height = loc.surfaceHeight;
        }
    }
    if (racer.airborne) {
        const float before = racer.height;
        racer.verticalSpeed -= GRAVITY * dt;
        racer.height += racer.verticalSpeed * dt;
        // Land on road that's below us now, but not road we passed under (we're falling past it)
        if (loc.hasSurface && racer.height <= loc.surfaceHeight && before >= loc.surfaceHeight - 0.5f && racer.mode == RacerMode::Driving) {
            if (racer.verticalSpeed < -12.0f) {
                racer.velocity *= 0.93f; // hard landing
            }
            racer.height = loc.surfaceHeight;
            racer.verticalSpeed = 0.0f;
            racer.climbRate = 0.0f;
            racer.airborne = false;
        }
    }

    // Tunnel roof: no jumping through it
    if (racer.mode == RacerMode::Driving && loc.tunnel && loc.hasSurface && racer.height > loc.surfaceHeight + TUNNEL_CLEARANCE - 1.5f) {
        racer.height = loc.surfaceHeight + TUNNEL_CLEARANCE - 1.5f;
        racer.verticalSpeed = std::min(racer.verticalSpeed, 0.0f);
    }

    // Barriers: only when low enough to hit them (tunnel walls go all the way up)
    if (racer.mode == RacerMode::Driving && (loc.tunnel || racer.height - loc.surfaceHeight < WALL_HEIGHT)) {
        const float limit = loc.halfWidth - CRAFT_HALF_WIDTH;
        const bool leftHit = loc.wallLeft && loc.lateral < -limit && loc.lateral > -loc.halfWidth - 4.0f;
        const bool rightHit = loc.wallRight && loc.lateral > limit && loc.lateral < loc.halfWidth + 4.0f;
        if (leftHit || rightHit) {
            const Vec2 trackRight = rightOf(loc.direction);
            const float outward = leftHit ? -1.0f : 1.0f;
            const float excess = std::fabs(loc.lateral) - limit;
            racer.position -= trackRight * (outward * excess);
            const float lateralSpeed = dot(racer.velocity, trackRight) * outward;
            if (lateralSpeed > 0.0f) {
                racer.velocity -= trackRight * (outward * lateralSpeed * (1.0f + WALL_BOUNCE));
                racer.velocity *= WALL_SPEED_LOSS + (1.0f - WALL_SPEED_LOSS) * (1.0f - clampf(lateralSpeed / 20.0f, 0.0f, 1.0f));
                racer.wallHitTime = 0.0f;
            }
            racer.location = track_->locate(racer.position, racer.location.segment);
        }
    }

    // Lost: fell below the track
    if (racer.mode == RacerMode::Driving && racer.height < FALL_LIMIT) {
        racer.mode = RacerMode::Falling;
        racer.modeTime = 0.0f;
        racer.falls++;
    } else if (racer.mode == RacerMode::Falling && racer.modeTime >= FALL_DURATION) {
        racer.mode = RacerMode::Respawning;
        racer.modeTime = 0.0f;
        racer.velocity = {};
    }

    // Wrong way
    if (racer.mode == RacerMode::Driving && dot(racer.velocity, racer.location.direction) < -3.0f) {
        racer.wrongWayTime += dt;
    } else {
        racer.wrongWayTime = 0.0f;
    }

    if (racer.mode == RacerMode::Driving) {
        updateProgress(racer, previousDistance);
    }
}

void RaceSim::updateProgress(Racer& racer, float previousDistance) {
    const float length = track_->length();
    const float distance = racer.location.distance;
    if (distance > length * 0.4f && distance < length * 0.6f) {
        racer.passedHalfway = true;
    }
    if (previousDistance > length * 0.75f && distance < length * 0.25f) {
        // Crossed the line going forwards. From the grid it counts without passing halfway.
        if (racer.passedHalfway || racer.lapsCompleted < 0) {
            racer.lapsCompleted++;
            racer.passedHalfway = false;
            if (racer.lapsCompleted >= 1 && !racer.finished) {
                racer.lastLapTime = time_ - racer.lapStartTime;
                if (racer.bestLapTime <= 0.0f || racer.lastLapTime < racer.bestLapTime) {
                    racer.bestLapTime = racer.lastLapTime;
                }
            }
            racer.lapStartTime = time_;
            if (racer.lapsCompleted >= laps_ && !racer.finished && phase_ == RacePhase::Racing) {
                racer.finished = true;
                racer.finishTime = time_;
            }
        }
    } else if (previousDistance < length * 0.25f && distance > length * 0.75f) {
        // Crossed the line going backwards
        racer.lapsCompleted--;
        racer.passedHalfway = true;
    }
    if (!racer.finished) {
        racer.raceDistance = (float)racer.lapsCompleted * length + distance;
    }
}

void RaceSim::respawn(Racer& racer) {
    // Back on the centreline at the last safe spot before where we fell
    const int safe = track_->safeSampleAtOrBefore(racer.location.segment);
    const TrackSample& sample = track_->sample(safe);
    const float previousDistance = racer.location.distance;
    racer.position = sample.position;
    racer.heading = directionToHeading(sample.direction);
    racer.velocity = sample.direction * 8.0f; // a little rolling start
    racer.height = sample.height;
    racer.verticalSpeed = 0.0f;
    racer.climbRate = 0.0f;
    racer.airborne = false;
    racer.mode = RacerMode::Driving;
    racer.modeTime = 0.0f;
    racer.ghostTime = GHOST_DURATION;
    racer.location = track_->locate(racer.position, safe);
    updateProgress(racer, previousDistance);
}

void RaceSim::collideRacers() {
    for (size_t i = 0; i < racers_.size(); i++) {
        Racer& a = racers_[i];
        if (a.mode != RacerMode::Driving || a.ghostTime > 0.0f) {
            continue;
        }
        for (size_t j = i + 1; j < racers_.size(); j++) {
            Racer& b = racers_[j];
            if (b.mode != RacerMode::Driving || b.ghostTime > 0.0f || std::fabs(a.height - b.height) > 1.5f) {
                continue;
            }
            const Vec2 offset = b.position - a.position;
            const float d2 = lengthSquared(offset);
            const float minDistance = CRAFT_RADIUS * 2.0f;
            if (d2 >= minDistance * minDistance || d2 < 1e-6f) {
                continue;
            }
            const float d = std::sqrt(d2);
            const Vec2 normal = offset * (1.0f / d);
            const float massA = vehicle(a).mass;
            const float massB = vehicle(b).mass;
            const float total = massA + massB;
            // Push apart, the lighter one further
            const float overlap = minDistance - d;
            a.position -= normal * (overlap * massB / total);
            b.position += normal * (overlap * massA / total);
            // Bounce along the contact normal
            const float closing = dot(a.velocity - b.velocity, normal);
            if (closing > 0.0f) {
                const float impulse = closing * 1.5f / total; // restitution 0.5
                a.velocity -= normal * (impulse * massB);
                b.velocity += normal * (impulse * massA);
            }
        }
    }
}

void RaceSim::rank() {
    std::vector<Racer*> order;
    for (auto& racer : racers_) {
        order.push_back(&racer);
    }
    std::stable_sort(order.begin(), order.end(), [](const Racer* a, const Racer* b) {
        if (a->finished != b->finished) return a->finished;
        if (a->finished && a->finishTime != b->finishTime) return a->finishTime < b->finishTime;
        return a->raceDistance > b->raceDistance;
    });
    for (size_t i = 0; i < order.size(); i++) {
        order[i]->place = (int)i + 1;
    }
}

} // namespace hd
