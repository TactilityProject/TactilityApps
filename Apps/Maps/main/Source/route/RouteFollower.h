// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GrowArray.h"
#include "RouteSearch.h"
#include "RouteTurn.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** A turn of the route: where it is (distance from the start) and onto which road. */
struct Maneuver {
    float atM;
    Turn turn;
    /** The step it turns onto (its name and class), -1 for the arrival. */
    int step;
};

/** Finds the turns of a route from the angles at its steps (M5c-4). Straight continuations are left out. */
void routeManeuvers(const Route& route, GrowArray<Maneuver>& out);
/** The turn for a change of direction in degrees (positive: to the right). */
Turn turnFor(float degrees);
/** "turn left", "keep slightly right", ... for an instruction. */
const char* turnPhrase(Turn turn);

/**
 * Follows a route with positions (M5c-4): snaps a position to the route within accuracy + MATCH_METRES, notices when
 * it has been more than OFF_ROUTE_METRES (or accuracy + 25 m) away for OFF_ROUTE_MS, finds the next turn and arrival.
 */
class RouteFollower {
public:
    static constexpr float MATCH_METRES = 20.0f;
    static constexpr float OFF_ROUTE_METRES = 40.0f;
    static constexpr uint32_t OFF_ROUTE_MS = 10000;
    static constexpr float ARRIVE_METRES = 25.0f;

    enum class State : uint8_t { Waiting, OnRoute, Away, OffRoute, Arrived };

    struct Status {
        State state;
        /** Distance along the route of the matched point, and that point. */
        float progressM;
        RoutePoint matched;
        float distanceFromRouteM;
        float remainingM;
        float remainingS;
        Turn nextTurn;
        float toTurnM;
        /** The step the next turn goes onto, -1 for the arrival. */
        int nextStep;
    };

    /** Starts following a route; the maneuvers are worked out once here. */
    void begin(const Route& route);
    void end() { active = false; }
    bool isActive() const { return active; }
    /** A new position (accuracy in metres) at time now. */
    const Status& update(const Route& route, double lat, double lon, float accuracyM, uint32_t now);
    const Status& status() const { return current; }
    /** The text of the next instruction, e.g. "In 120 m turn left onto Štefánikova". */
    void instruction(const Route& route, char* out, size_t size) const;
    /** "2.1 km · 28 min left". */
    void remaining(char* out, size_t size) const;

private:
    bool active = false;
    GrowArray<Maneuver> maneuvers;
    Status current {};
    /** The segment matched last, where the next match looks first. */
    size_t lastSegment = 0;
    uint32_t awaySince = 0;
    bool away = false;
};

/** "120 m", "850 m", "2.4 km". */
void formatDistance(float metres, char* out, size_t size);
/** "4 min", "1 h 05 min". */
void formatDuration(float seconds, char* out, size_t size);

} // namespace maps
