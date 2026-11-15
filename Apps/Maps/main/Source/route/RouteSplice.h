// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "RouteSearch.h"

namespace maps {

/**
 * Off a long route, a new search to the destination takes seconds, and on a bike or a bus the position has moved on
 * by the time it is done (the new route starts behind, is left again, and so on). So the search goes only as far as
 * the old route a little ahead, and the rest of the old route is kept (0.6.0).
 *
 * The point of the old route to steer for: the first one at least aheadM beyond where the position is along the
 * route. That is the point of the route nearest to the position from progressM on (not progressM itself: someone
 * who went on along another road is level with a later part of the route, and must not be sent back to where they
 * left it). SIZE_MAX when less than minLeftM of the route would be left after it: then a search to the destination
 * is as short.
 */
size_t rejoinTarget(const Route& old, const RoutePoint& position, float progressM, float aheadM, float minLeftM);

/**
 * Whether going to the join point is a step towards the destination: false when the position is already nearer to
 * the destination (in a straight line) than the join point by more than a third of the way to it, which is a way
 * back, not a way on.
 */
bool rejoinLeadsOn(const RoutePoint& position, const RoutePoint& join, const RoutePoint& destination, float pieceM);

/**
 * Joins a found piece, which ends at old.points[target], with the old route from there on. Where the piece comes
 * back along the old route before it ends (it met the route further ahead and ran back to the target), that spur
 * is cut: the join is where the two first meet. False, and out empty, when the piece does not end at the target or
 * memory ran out.
 */
bool spliceRoute(const Route& piece, const Route& old, size_t target, Route& out);

} // namespace maps
