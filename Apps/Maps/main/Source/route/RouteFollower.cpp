// SPDX-License-Identifier: GPL-3.0-or-later
#include "RouteFollower.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace maps {

namespace {

constexpr float PI_F = 3.14159265f;
/** Directions at a turn are measured over this many metres before and after it, so a kink in the drawing is no turn. */
constexpr float BEARING_METRES = 15.0f;

/** Compass bearing in degrees of the step from a to b (graph y grows southwards). */
float bearingOf(const RoutePoint& a, const RoutePoint& b) {
    return std::atan2(static_cast<float>(b.x - a.x), static_cast<float>(a.y - b.y)) * 180.0f / PI_F;
}

float segmentLength(const RoutePoint& a, const RoutePoint& b) {
    const float dx = static_cast<float>(b.x - a.x);
    const float dy = static_cast<float>(b.y - a.y);
    return std::sqrt(dx * dx + dy * dy);
}

} // namespace

Turn turnFor(float degrees) {
    const float angle = std::fabs(degrees);
    const bool right = degrees > 0.0f;
    if (angle < 25.0f) {
        return Turn::Straight;
    }
    if (angle < 50.0f) {
        return right ? Turn::SlightRight : Turn::SlightLeft;
    }
    if (angle < 120.0f) {
        return right ? Turn::Right : Turn::Left;
    }
    if (angle < 160.0f) {
        return right ? Turn::SharpRight : Turn::SharpLeft;
    }
    return Turn::UTurn;
}

const char* turnPhrase(Turn turn) {
    switch (turn) {
        case Turn::SlightLeft:
            return "keep left";
        case Turn::Left:
            return "turn left";
        case Turn::SharpLeft:
            return "turn sharp left";
        case Turn::SlightRight:
            return "keep right";
        case Turn::Right:
            return "turn right";
        case Turn::SharpRight:
            return "turn sharp right";
        case Turn::UTurn:
            return "turn around";
        case Turn::Arrive:
            return "arrive";
        case Turn::Roundabout:
            return "enter the roundabout";
        default:
            return "continue";
    }
}

void routeManeuvers(const Route& route, GrowArray<Maneuver>& out) {
    out.clear();
    const size_t count = route.points.size();
    for (size_t s = 1; s < route.steps.size(); s++) {
        const size_t at = std::min<size_t>(route.steps[s].point, count - 1);
        if (at == 0 || at + 1 >= count) {
            continue;
        }
        // A point some metres before and after the junction
        size_t before = at;
        while (before > 0 && route.distances[at] - route.distances[before] < BEARING_METRES) {
            before--;
        }
        size_t after = at;
        while (after + 1 < count && route.distances[after] - route.distances[at] < BEARING_METRES) {
            after++;
        }
        if (before == at || after == at) {
            continue;
        }
        float change = bearingOf(route.points[at], route.points[after]) - bearingOf(route.points[before], route.points[at]);
        while (change > 180.0f) {
            change -= 360.0f;
        }
        while (change <= -180.0f) {
            change += 360.0f;
        }
        const Turn turn = turnFor(change);
        // Where the road just goes on (another name, the same direction) no instruction is given
        if (turn == Turn::Straight) {
            continue;
        }
        out.push({ route.distances[at], turn, static_cast<int>(s) });
    }
    out.push({ route.lengthM, Turn::Arrive, -1 });
}

void RouteFollower::begin(const Route& route) {
    routeManeuvers(route, maneuvers);
    current = {};
    current.state = State::Waiting;
    current.remainingM = route.lengthM;
    current.remainingS = route.seconds;
    current.nextTurn = maneuvers.size() > 0 ? maneuvers[0].turn : Turn::Arrive;
    current.toTurnM = maneuvers.size() > 0 ? maneuvers[0].atM : route.lengthM;
    current.nextStep = maneuvers.size() > 0 ? maneuvers[0].step : -1;
    lastSegment = 0;
    away = false;
    awaySince = 0;
    active = route.points.size() >= 2;
}

const RouteFollower::Status& RouteFollower::update(const Route& route, double lat, double lon, float accuracyM, uint32_t now) {
    if (!active || route.points.size() < 2 || current.state == State::Arrived) {
        return current;
    }
    const RoutePoint here = routePointFrom(lat, lon);
    const auto metresPerUnit = static_cast<float>(routeMetresPerUnit(here.y));
    const size_t segments = route.points.size() - 1;
    // Nearest segment: near the last match first, the whole route when that finds nothing close
    auto nearest = [&](size_t first, size_t last, size_t& bestSegment, float& bestT, float& bestDistance) {
        bestDistance = INFINITY;
        for (size_t i = first; i < last; i++) {
            const RoutePoint& a = route.points[i];
            const RoutePoint& b = route.points[i + 1];
            const float dx = static_cast<float>(b.x - a.x);
            const float dy = static_cast<float>(b.y - a.y);
            const float lengthSquared = dx * dx + dy * dy;
            float t = lengthSquared > 0.0f ? ((here.x - a.x) * dx + (here.y - a.y) * dy) / lengthSquared : 0.0f;
            t = std::clamp(t, 0.0f, 1.0f);
            const float px = a.x + t * dx - here.x;
            const float py = a.y + t * dy - here.y;
            float distance = std::sqrt(px * px + py * py) * metresPerUnit;
            // A route that passes the same street twice: going back along it counts a little more
            if (i + 2 < lastSegment) {
                distance += 5.0f;
            }
            if (distance < bestDistance) {
                bestDistance = distance;
                bestSegment = i;
                bestT = t;
            }
        }
    };
    size_t segment = 0;
    float t = 0.0f;
    float distance = INFINITY;
    const size_t windowFirst = lastSegment > 20 ? lastSegment - 20 : 0;
    const size_t windowLast = std::min(segments, lastSegment + 400);
    nearest(windowFirst, windowLast, segment, t, distance);
    const float matchLimit = accuracyM + MATCH_METRES;
    if (distance > matchLimit && (windowFirst > 0 || windowLast < segments)) {
        nearest(0, segments, segment, t, distance);
    }
    const float segmentMetres = route.distances[segment + 1] - route.distances[segment];
    const float progress = route.distances[segment] + t * segmentMetres;
    current.distanceFromRouteM = distance;
    const float offLimit = std::max(OFF_ROUTE_METRES, accuracyM + 25.0f);
    if (distance <= matchLimit) {
        away = false;
        lastSegment = segment;
        current.progressM = progress;
        const RoutePoint& a = route.points[segment];
        const RoutePoint& b = route.points[segment + 1];
        current.matched = { a.x + static_cast<int32_t>((b.x - a.x) * t), a.y + static_cast<int32_t>((b.y - a.y) * t) };
        current.state = State::OnRoute;
    } else if (distance > offLimit) {
        if (!away) {
            away = true;
            awaySince = now;
        }
        current.state = now - awaySince >= OFF_ROUTE_MS ? State::OffRoute : State::Away;
    }
    // Arrival: close to the goal, or the end of the route reached on it
    const RoutePoint& goal = route.points[route.points.size() - 1];
    const float toGoal = segmentLength(here, goal) * metresPerUnit;
    if (toGoal <= ARRIVE_METRES || (current.state == State::OnRoute && route.lengthM - current.progressM <= ARRIVE_METRES)) {
        current.state = State::Arrived;
        current.remainingM = 0.0f;
        current.remainingS = 0.0f;
        current.nextTurn = Turn::Arrive;
        current.toTurnM = 0.0f;
        current.nextStep = -1;
        return current;
    }
    current.remainingM = std::max(0.0f, route.lengthM - current.progressM);
    current.remainingS = route.lengthM > 0.0f ? route.seconds * current.remainingM / route.lengthM : 0.0f;
    for (const Maneuver& maneuver : maneuvers) {
        if (maneuver.atM > current.progressM + 1.0f || maneuver.turn == Turn::Arrive) {
            current.nextTurn = maneuver.turn;
            current.toTurnM = std::max(0.0f, maneuver.atM - current.progressM);
            current.nextStep = maneuver.step;
            break;
        }
    }
    return current;
}

void formatDistance(float metres, char* out, size_t size) {
    if (metres < 100.0f) {
        snprintf(out, size, "%d m", static_cast<int>(std::floor(metres / 10.0f + 0.5f)) * 10);
    } else if (metres < 1000.0f) {
        snprintf(out, size, "%d m", static_cast<int>(std::floor(metres / 50.0f + 0.5f)) * 50);
    } else if (metres < 10000.0f) {
        snprintf(out, size, "%.1f km", metres / 1000.0f);
    } else {
        snprintf(out, size, "%d km", static_cast<int>(std::floor(metres / 1000.0f + 0.5f)));
    }
}

void formatDuration(float seconds, char* out, size_t size) {
    const int minutes = std::max(1, static_cast<int>(std::floor(seconds / 60.0f + 0.5f)));
    if (minutes < 60) {
        snprintf(out, size, "%d min", minutes);
    } else {
        snprintf(out, size, "%d h %02d min", minutes / 60, minutes % 60);
    }
}

void RouteFollower::instruction(const Route& route, char* out, size_t size) const {
    if (current.state == State::Arrived) {
        snprintf(out, size, "You have arrived");
        return;
    }
    char distance[16];
    formatDistance(current.toTurnM, distance, sizeof(distance));
    if (current.nextTurn == Turn::Arrive) {
        snprintf(out, size, "In %s you arrive", distance);
        return;
    }
    char name[64] = "";
    if (current.nextStep >= 0 && static_cast<size_t>(current.nextStep) < route.steps.size()) {
        route.stepName(route.steps[current.nextStep], name, sizeof(name));
    }
    const char* phrase = turnPhrase(current.nextTurn);
    const bool named = name[0] != '\0';
    if (current.toTurnM < 15.0f && named) {
        snprintf(out, size, "Now %s onto %s", phrase, name);
    } else if (current.toTurnM < 15.0f) {
        snprintf(out, size, "Now %s", phrase);
    } else if (named) {
        snprintf(out, size, "In %s %s onto %s", distance, phrase, name);
    } else {
        snprintf(out, size, "In %s %s", distance, phrase);
    }
}

void RouteFollower::remaining(char* out, size_t size) const {
    char distance[16];
    char time[24];
    formatDistance(current.remainingM, distance, sizeof(distance));
    formatDuration(current.remainingS, time, sizeof(time));
    snprintf(out, size, "%s \xC2\xB7 %s left", distance, time);
}

} // namespace maps
