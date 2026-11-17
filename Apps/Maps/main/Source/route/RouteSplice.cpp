// SPDX-License-Identifier: GPL-3.0-or-later
#include "RouteSplice.h"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace maps {

namespace {

/** Two route points are the same place: the search's points come from the same tile shapes, a snapped end a little off. */
bool near(const RoutePoint& a, const RoutePoint& b, int32_t units) {
    const int64_t dx = static_cast<int64_t>(a.x) - b.x;
    const int64_t dy = static_cast<int64_t>(a.y) - b.y;
    return dx * dx + dy * dy <= static_cast<int64_t>(units) * units;
}

float metresBetween(const RoutePoint& a, const RoutePoint& b) {
    const float dx = static_cast<float>(b.x - a.x);
    const float dy = static_cast<float>(b.y - a.y);
    return std::sqrt(dx * dx + dy * dy) * static_cast<float>(routeMetresPerUnit((a.y + b.y) / 2));
}

/** Where a snapped end may lie from the point that was asked for: a few metres. */
constexpr int32_t END_UNITS = 16;
constexpr int32_t SAME_UNITS = 2;

} // namespace

size_t rejoinTarget(const Route& old, const RoutePoint& position, float progressM, float aheadM, float minLeftM) {
    const size_t count = old.points.size();
    if (count < 2 || old.distances.size() != count || old.track) {
        return SIZE_MAX;
    }
    // Level with the position: the nearest point of the route from the progress on
    float level = progressM;
    {
        int64_t best = INT64_MAX;
        for (size_t i = 0; i < count; i++) {
            if (old.distances[i] < progressM) {
                continue;
            }
            const int64_t dx = static_cast<int64_t>(old.points[i].x) - position.x;
            const int64_t dy = static_cast<int64_t>(old.points[i].y) - position.y;
            const int64_t squared = dx * dx + dy * dy;
            if (squared < best) {
                best = squared;
                level = old.distances[i];
            }
        }
    }
    const float wanted = level + aheadM;
    // The distances grow along the route: the first point beyond the wanted one
    size_t low = 0;
    size_t high = count - 1;
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (old.distances[middle] < wanted) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (old.distances[low] < wanted || old.lengthM - old.distances[low] < minLeftM) {
        return SIZE_MAX;
    }
    return low;
}

bool rejoinLeadsOn(const RoutePoint& position, const RoutePoint& join, const RoutePoint& destination, float pieceM) {
    return metresBetween(join, destination) <= metresBetween(position, destination) + pieceM / 3.0f;
}

bool spliceRoute(const Route& piece, const Route& old, size_t target, Route& out) {
    out.clear();
    const size_t pieceCount = piece.points.size();
    const size_t oldCount = old.points.size();
    if (pieceCount < 2 || target >= oldCount || !near(piece.points[pieceCount - 1], old.points[target], END_UNITS)) {
        return false;
    }
    // The spur: the piece's last points are the old route's next ones, walked backwards
    size_t last = pieceCount - 1;
    size_t join = target;
    while (last > 0 && join + 1 < oldCount && near(piece.points[last - 1], old.points[join + 1], SAME_UNITS)) {
        last--;
        join++;
    }
    if (last == 0) {
        // The whole piece lies on the old route (the position was on it after all): the old route from there
        last = 0;
    }
    // Points: the piece up to the join, the old route after it
    for (size_t i = 0; i <= last; i++) {
        out.points.push(piece.points[i]);
    }
    for (size_t i = join + 1; i < oldCount; i++) {
        out.points.push(old.points[i]);
    }
    if (out.points.failed() || out.points.size() < 2) {
        out.clear();
        return false;
    }
    float distance = 0.0f;
    for (size_t i = 0; i < out.points.size(); i++) {
        if (i > 0) {
            distance += metresBetween(out.points[i - 1], out.points[i]);
        }
        out.distances.push(distance);
    }
    // Steps: the piece's up to the join, then the old route's from the road it is on at the join
    for (char c : piece.names) {
        out.names.push(c);
    }
    const size_t nameShift = piece.names.size();
    for (char c : old.names) {
        out.names.push(c);
    }
    if (out.names.size() > UINT16_MAX) {
        out.clear();
        return false;
    }
    const RouteStep* lastKept = nullptr;
    for (const RouteStep& step : piece.steps) {
        if (step.point > last) {
            break;
        }
        out.steps.push(step);
        lastKept = &step;
    }
    const RouteStep* active = nullptr;
    for (const RouteStep& step : old.steps) {
        if (step.point <= join) {
            active = &step;
        }
    }
    auto sameRoad = [&](const RouteStep& a, const RouteStep& b) {
        // The same coarseness too: a coarse step (main-road layer) keeps its own wider off-route limit
        return a.roadClass == b.roadClass && (a.flags & ROUTE_STEP_COARSE) == (b.flags & ROUTE_STEP_COARSE) && a.nameLength == b.nameLength &&
            (a.nameLength == 0 || (a.nameOffset + a.nameLength <= piece.names.size() && b.nameOffset + b.nameLength <= old.names.size() &&
                memcmp(piece.names.begin() + a.nameOffset, old.names.begin() + b.nameOffset, a.nameLength) == 0));
    };
    if (active != nullptr && (lastKept == nullptr || !sameRoad(*lastKept, *active))) {
        // Another road from the join on: its step starts there
        RouteStep step = *active;
        step.point = static_cast<uint32_t>(last);
        step.nameOffset = static_cast<uint16_t>(step.nameOffset + nameShift);
        out.steps.push(step);
    }
    for (const RouteStep& oldStep : old.steps) {
        if (oldStep.point <= join) {
            continue;
        }
        RouteStep step = oldStep;
        step.point = static_cast<uint32_t>(oldStep.point - join + last);
        step.nameOffset = static_cast<uint16_t>(step.nameOffset + nameShift);
        out.steps.push(step);
    }
    if (out.distances.failed() || out.names.failed() || out.steps.failed()) {
        out.clear();
        return false;
    }
    out.profile = old.profile;
    out.track = false;
    out.from = piece.from;
    out.to = old.to;
    out.lengthM = distance;
    // The time: each part's own speed, by the share of its length that is kept
    const float pieceKept = out.distances[last];
    const float oldLeft = old.distances.size() == oldCount ? old.lengthM - old.distances[join] : 0.0f;
    out.seconds = (piece.lengthM > 0.0f ? piece.seconds * std::min(1.0f, pieceKept / piece.lengthM) : 0.0f) +
        (old.lengthM > 0.0f ? old.seconds * std::max(0.0f, oldLeft) / old.lengthM : 0.0f);
    return true;
}

} // namespace maps
