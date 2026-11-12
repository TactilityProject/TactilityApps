// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RouteGraph.h"

#include <cstdint>

namespace maps {

/** Walk (the default), bike, car. Values are stored in settings and route files: only add at the end. */
enum class RouteProfile : uint8_t { Walk, Bike, Car, Count };

const char* routeProfileName(RouteProfile profile);

/**
 * Speeds for the time estimate and the search cost, in km/h: one table for all profiles. Walk and bike use one speed
 * for every road; car uses the road class.
 */
constexpr float WALK_KMH = 4.5f;
constexpr float BIKE_KMH = 15.0f;
constexpr float CAR_KMH[static_cast<int>(RoadClass::Count)] = {
    110.0f, // motorway
    90.0f,  // trunk
    70.0f,  // primary
    60.0f,  // secondary
    50.0f,  // tertiary
    40.0f,  // minor
    20.0f,  // service
    20.0f,  // track (not used by car)
    5.0f,   // path (not used by car)
};
/** The speed the car search assumes for the straight line to the goal (A* weighted a little: see routeMaxKmh). */
constexpr float CAR_ESTIMATE_KMH = 70.0f;
/** The speed of a profile for the search's straight-line estimate. */
float routeMaxKmh(RouteProfile profile);

/** Whether a profile may use an edge at all: bike no motorway or trunk; car no path (footways, steps) or track. */
inline bool routeAllows(RouteProfile profile, uint8_t roadClass, uint8_t flags) {
    (void)flags;
    switch (profile) {
        case RouteProfile::Bike:
            return roadClass != static_cast<uint8_t>(RoadClass::Motorway) && roadClass != static_cast<uint8_t>(RoadClass::Trunk);
        case RouteProfile::Car:
            return roadClass != static_cast<uint8_t>(RoadClass::Path) && roadClass != static_cast<uint8_t>(RoadClass::Track);
        default:
            return true;
    }
}

/** Whether an edge may be used against its direction: oneway counts for bike and car, walking ignores it. */
inline bool routeAllowsBackwards(RouteProfile profile, uint8_t flags) {
    return profile == RouteProfile::Walk || (flags & EDGE_ONEWAY) == 0;
}

/** Seconds to travel lengthM metres on a road of this class. */
float routeSeconds(RouteProfile profile, uint8_t roadClass, float lengthM);

} // namespace maps
