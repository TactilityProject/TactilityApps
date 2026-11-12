// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "GrowArray.h"
#include "RouteSearch.h"
#include "RouteTurn.h"

#include <cmath>
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
 * When a position counts as off the route (0.4.1). The threshold follows the GPS precision: the median accuracy of the
 * fixes of the last windowMs gives acc, and off = clamp(accuracyFactor * acc + accuracyAddM, minOffM, maxOffM). A fix is
 * away when it is farther than off from the route, or farther than headingShare * off while moving faster than
 * headingSpeed in a direction more than headingDegrees from the route's, or (edge rule) nearer to another road than to
 * the route by edgeMarginM while farther than edgeShare * off. triggerFixes away fixes in a row are off route. Above
 * weakAccuracyM nothing triggers ("Weak GPS"). The values can be changed by the "offroute" debug command.
 */
struct OffRouteTuning {
    float accuracyFactor = 1.5f;
    float accuracyAddM = 5.0f;
    float minOffM = 15.0f;
    float maxOffM = 60.0f;
    /** The accuracy of a fix that reports none (Tactility gives HDOP x 5 m when the GPS sends HDOP). */
    float noAccuracyM = 30.0f;
    float weakAccuracyM = 50.0f;
    uint32_t windowMs = 10000;
    int triggerFixes = 3;
    /**
     * Above poorFromM of accuracy, poorFixesPerM more fixes per metre are needed (25 m: 3 + 5 = 8): GPS noise of that size
     * alone crosses the threshold for 3 fixes now and then on a long walk (PC replays, MEASUREMENTS.md "0.4.1").
     */
    float poorFromM = 10.0f;
    float poorFixesPerM = 0.33f;
    float headingSpeed = 1.0f;
    float headingDegrees = 60.0f;
    float headingShare = 0.667f;
    /** The edge rule: 0 turns it off. */
    int edgeRule = 1;
    float edgeShare = 0.667f;
    float edgeMarginM = 4.0f;
    /** The edge rule only with GPS this good: with poor GPS the noise alone puts fixes nearer to side streets. */
    float edgeMaxAccuracyM = 10.0f;
    /** At least this long between two recalculations after the first one. */
    uint32_t recalculateMs = 10000;
    /** Fixes not checked after a new route, so it settles. */
    int settleFixes = 2;
    /** The heading and edge rules count only past this many metres along a route (device replays: false alarms at the start). */
    float startGuardM = 30.0f;
};

/** The tuning in use, shared by the follower and the map (the debug script changes it). */
OffRouteTuning& offRouteTuning();
/** Sets one tuning value by its name ("minOffM", "triggerFixes", ...). False for an unknown name. */
bool setOffRouteTuning(const char* name, float value);
/** The threshold for a median accuracy: clamp(factor * acc + add, min, max). */
float offRouteThreshold(const OffRouteTuning& tuning, float accuracyM);
/** The away fixes in a row that make off route at a median accuracy. */
int offRouteFixes(const OffRouteTuning& tuning, float accuracyM);

/** The median of the accuracies of the last window of fixes; a single bad fix doesn't move it. */
class AccuracyWindow {
public:
    static constexpr int MAX = 32;
    void clear() { count = 0; }
    /** Adds a fix's accuracy (NAN or <= 0: noAccuracyM) at time now. */
    void add(float accuracyM, uint32_t now, const OffRouteTuning& tuning);
    float median() const;
    int size() const { return count; }

private:
    float values[MAX] {};
    uint32_t times[MAX] {};
    int count = 0;
};

enum class OffRouteRule : uint8_t { None, Distance, Heading, Edge };
const char* offRouteRuleName(OffRouteRule rule);

/**
 * Follows a route with positions (M5c-4, 0.4.1): projects a position onto the route, notices when it left the route
 * (OffRouteTuning), finds the next turn and arrival.
 */
class RouteFollower {
public:
    static constexpr float MATCH_METRES = 20.0f;
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
        /** The median accuracy and the off-route threshold used for the last fix. */
        float accuracyM;
        float offM;
        /** Course against the route's direction in degrees, NAN without a course. */
        float headingDifference;
        /** Distance to the nearest other road (Route::sides), INFINITY without one. */
        float sideM;
        /** The rule that counted the last fix as away, and the one that made it off route. */
        OffRouteRule awayRule;
        OffRouteRule firedRule;
        uint8_t awayFixes;
        bool weakGps;
    };

    /** Starts following a route; the maneuvers are worked out once here. */
    void begin(const Route& route);
    void end() { active = false; }
    bool isActive() const { return active; }
    /**
     * A new position at time now: accuracy in metres (NAN: unknown), course in degrees and speed in m/s (NAN: unknown).
     * Each call counts as one fix for the off-route rule.
     */
    const Status& update(const Route& route, double lat, double lon, float accuracyM, uint32_t now, float courseDeg = NAN,
        float speedMps = NAN);
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
    int awayFixes = 0;
    int settleLeft = 0;
    AccuracyWindow accuracy;
};

/** "120 m", "850 m", "2.4 km". */
void formatDistance(float metres, char* out, size_t size);
/** "4 min", "1 h 05 min". */
void formatDuration(float seconds, char* out, size_t size);

} // namespace maps
