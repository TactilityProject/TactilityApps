// SPDX-License-Identifier: GPL-3.0-or-later
// The roads next to a route (0.4.1): segments of other roads near the route, for the follower's edge rule.
#include "RouteSearch.h"

#include <algorithm>
#include <cmath>

namespace maps {

namespace {

/** Route points per chunk; each chunk has a box, so a test looks only at the few chunks nearby. */
constexpr size_t CHUNK_POINTS = 16;
/** A road segment is cut into pieces of at most this many metres before it is tested. */
constexpr float PIECE_METRES = 15.0f;
/** A piece this near the route is part of it. */
constexpr float ON_ROUTE_METRES = 3.0f;
/** A piece at less than this angle to the route runs beside it (a separate pavement, a parallel lane): not kept. */
constexpr float PARALLEL_DEGREES = 30.0f;

struct Chunk {
    size_t first;
    size_t last;
    int32_t minX;
    int32_t minY;
    int32_t maxX;
    int32_t maxY;
};

float pointSegment(float px, float py, const RoutePoint& a, const RoutePoint& b, float& t) {
    const float dx = static_cast<float>(b.x - a.x);
    const float dy = static_cast<float>(b.y - a.y);
    const float lengthSquared = dx * dx + dy * dy;
    t = lengthSquared > 0.0f ? ((px - a.x) * dx + (py - a.y) * dy) / lengthSquared : 0.0f;
    t = std::clamp(t, 0.0f, 1.0f);
    const float ex = a.x + t * dx - px;
    const float ey = a.y + t * dy - py;
    return std::sqrt(ex * ex + ey * ey);
}

} // namespace

bool RouteSearch::addSides(GraphSource& source, const GrowArray<RoutePoint>& points, GrowArray<RouteSide>& sides) {
    sides.clear();
    const size_t count = points.size();
    if (count < 2) {
        return false;
    }
    if (!inflaterReady) {
        inflaterReady = inflater.init();
        if (!inflaterReady) {
            return false;
        }
    }
    const float metresPerUnit = static_cast<float>(routeMetresPerUnit(points[0].y));
    const auto radius = static_cast<int32_t>(SIDE_METRES / metresPerUnit) + 1;
    const float onRoute = ON_ROUTE_METRES / metresPerUnit;
    const float pieceUnits = PIECE_METRES / metresPerUnit;
    const float parallelCos = std::cos(PARALLEL_DEGREES * 3.14159265f / 180.0f);

    GrowArray<Chunk> chunks;
    for (size_t first = 0; first + 1 < count; first += CHUNK_POINTS) {
        const size_t last = std::min(count - 1, first + CHUNK_POINTS);
        Chunk chunk { first, last, INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN };
        for (size_t i = first; i <= last; i++) {
            chunk.minX = std::min(chunk.minX, points[i].x);
            chunk.minY = std::min(chunk.minY, points[i].y);
            chunk.maxX = std::max(chunk.maxX, points[i].x);
            chunk.maxY = std::max(chunk.maxY, points[i].y);
        }
        chunk.minX -= radius;
        chunk.minY -= radius;
        chunk.maxX += radius;
        chunk.maxY += radius;
        chunks.push(chunk);
        if (chunks.failed()) {
            return false;
        }
    }
    // The nearest route segment to a point: its distance in units and the segment
    auto nearestOnRoute = [&](float px, float py, size_t& segment) {
        float best = INFINITY;
        for (const Chunk& chunk : chunks) {
            if (px < chunk.minX || px > chunk.maxX || py < chunk.minY || py > chunk.maxY) {
                continue;
            }
            for (size_t i = chunk.first; i < chunk.last; i++) {
                float t;
                const float d = pointSegment(px, py, points[i], points[i + 1], t);
                if (d < best) {
                    best = d;
                    segment = i;
                }
            }
        }
        return best;
    };

    // The tiles the route passes, and their neighbours where the route comes near a side
    GrowArray<TileKey> sideTiles;
    auto addTile = [&](int32_t tx, int32_t ty) {
        const TileKey key { 14, tx, ty };
        for (const TileKey& known : sideTiles) {
            if (known == key) {
                return;
            }
        }
        sideTiles.push(key);
    };
    for (size_t i = 0; i < count; i++) {
        const RoutePoint& p = points[i];
        for (int oy = -1; oy <= 1; oy++) {
            for (int ox = -1; ox <= 1; ox++) {
                addTile((p.x + ox * radius) / GRAPH_EXTENT, (p.y + oy * radius) / GRAPH_EXTENT);
            }
        }
    }

    TileGraph graph;
    for (const TileKey& tile : sideTiles) {
        if (sides.size() >= MAX_SIDES) {
            break;
        }
        if (cancelled) {
            // The navigator stops: the side roads are optional
            sides.clear();
            return false;
        }
        if (!source.readGraph(tile, fileBuffer) || !loadTileGraph(fileBuffer.begin(), fileBuffer.size(), inflater, graph)) {
            continue;
        }
        const int32_t baseX = tile.x * GRAPH_EXTENT;
        const int32_t baseY = tile.y * GRAPH_EXTENT;
        for (uint32_t e = 0; e < graph.edges.size() && sides.size() < MAX_SIDES; e++) {
            bool havePrevious = false;
            RoutePoint previous {};
            graph.shape(e, false, [&](int32_t x, int32_t y) {
                const RoutePoint point { baseX + x, baseY + y };
                if (havePrevious && sides.size() < MAX_SIDES) {
                    const float dx = static_cast<float>(point.x - previous.x);
                    const float dy = static_cast<float>(point.y - previous.y);
                    const float length = std::sqrt(dx * dx + dy * dy);
                    const int pieces = std::max(1, static_cast<int>(std::ceil(length / pieceUnits)));
                    for (int k = 0; k < pieces; k++) {
                        const RoutePoint a { previous.x + static_cast<int32_t>(dx * k / pieces), previous.y + static_cast<int32_t>(dy * k / pieces) };
                        const RoutePoint b { previous.x + static_cast<int32_t>(dx * (k + 1) / pieces),
                            previous.y + static_cast<int32_t>(dy * (k + 1) / pieces) };
                        const float mx = 0.5f * static_cast<float>(a.x + b.x);
                        const float my = 0.5f * static_cast<float>(a.y + b.y);
                        size_t segment = 0;
                        const float d = nearestOnRoute(mx, my, segment);
                        if (!(d <= radius) || d < onRoute) {
                            continue;
                        }
                        // Leaves out pieces running beside the route
                        const RoutePoint& ra = points[segment];
                        const RoutePoint& rb = points[segment + 1];
                        const float rx = static_cast<float>(rb.x - ra.x);
                        const float ry = static_cast<float>(rb.y - ra.y);
                        const float px = static_cast<float>(b.x - a.x);
                        const float py = static_cast<float>(b.y - a.y);
                        const float norms = std::sqrt((rx * rx + ry * ry) * (px * px + py * py));
                        if (norms > 0.0f && std::fabs(rx * px + ry * py) / norms > parallelCos) {
                            continue;
                        }
                        sides.push({ a, b });
                    }
                }
                previous = point;
                havePrevious = true;
            });
        }
    }
    graph.clear();
    return true;
}

} // namespace maps
