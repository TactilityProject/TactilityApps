// SPDX-License-Identifier: GPL-3.0-or-later
#include "Contours.h"

#include "../platform/Platform.h"
#include "Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace maps {

namespace {

// Sides of a cell: the edge it shares with the cell above, to the right, below, to the left
constexpr int TOP = 0;
constexpr int RIGHT = 1;
constexpr int BOTTOM = 2;
constexpr int LEFT = 3;

} // namespace

ContourTracer::~ContourTracer() {
    end();
}

bool ContourTracer::begin(int maxWidth, int maxHeight, int maxPointCount) {
    end();
    // Horizontal edges (width - 1) x height and vertical ones width x (height - 1), one bit each, in one array
    visitedBytes = (static_cast<size_t>(maxWidth) * maxHeight * 2 + 7) / 8;
    visited = static_cast<uint8_t*>(largeAlloc(visitedBytes));
    points = static_cast<float*>(largeAlloc(static_cast<size_t>(maxPointCount) * 2 * sizeof(float)));
    if (visited == nullptr || points == nullptr) {
        end();
        return false;
    }
    maxPoints = maxPointCount;
    capacityWidth = maxWidth;
    capacityHeight = maxHeight;
    return true;
}

void ContourTracer::end() {
    largeFree(visited);
    largeFree(points);
    visited = nullptr;
    points = nullptr;
    visitedBytes = 0;
    maxPoints = 0;
}

bool ContourTracer::trace(const uint16_t* grid, int width, int height, int stride, float intervalM, LineSink sink, void* context) {
    lines = 0;
    if (visited == nullptr || width < 2 || height < 2 || width > capacityWidth || height > capacityHeight || !(intervalM > 0.0f)) {
        return false;
    }
    uint16_t low = 65535;
    uint16_t high = 0;
    for (int y = 0; y < height; y++) {
        const uint16_t* row = grid + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; x++) {
            low = std::min(low, row[x]);
            high = std::max(high, row[x]);
        }
    }
    const float lowM = heightFromCode(low);
    const float highM = heightFromCode(high);
    for (float levelM = std::ceil(lowM / intervalM) * intervalM; levelM <= highM; levelM += intervalM) {
        // The level as a code: a corner is "above" when its code is at least this
        const float code = (levelM - TERRAIN_BASE_M) * 2.0f;
        if (code <= 0.0f || code > 65535.0f) {
            continue;
        }
        traceLevel(grid, width, height, stride, static_cast<uint16_t>(code), levelM, sink, context);
    }
    return true;
}

void ContourTracer::traceLevel(const uint16_t* grid, int width, int height, int stride, uint16_t level, float levelM, LineSink sink,
    void* context) {
    memset(visited, 0, visitedBytes);
    auto above = [&](int x, int y) { return grid[static_cast<size_t>(y) * stride + x] >= level; };
    // Open lines first, from every crossing on the border, so they are traced end to end
    for (int x = 0; x + 1 < width; x++) {
        if (above(x, 0) != above(x + 1, 0)) {
            follow(grid, width, height, stride, level, levelM, x, 0, TOP, sink, context);
        }
        if (above(x, height - 1) != above(x + 1, height - 1)) {
            follow(grid, width, height, stride, level, levelM, x, height - 2, BOTTOM, sink, context);
        }
    }
    for (int y = 0; y + 1 < height; y++) {
        if (above(0, y) != above(0, y + 1)) {
            follow(grid, width, height, stride, level, levelM, 0, y, LEFT, sink, context);
        }
        if (above(width - 1, y) != above(width - 1, y + 1)) {
            follow(grid, width, height, stride, level, levelM, width - 2, y, RIGHT, sink, context);
        }
    }
    // Then the closed ones: from any crossing not used yet
    for (int y = 1; y + 1 < height; y++) {
        for (int x = 0; x + 1 < width; x++) {
            if (above(x, y) != above(x + 1, y)) {
                follow(grid, width, height, stride, level, levelM, x, y, TOP, sink, context);
            }
        }
    }
}

void ContourTracer::follow(const uint16_t* grid, int width, int height, int stride, uint16_t level, float levelM, int cellX, int cellY,
    int entrySide, LineSink sink, void* context) {
    auto value = [&](int x, int y) { return static_cast<float>(grid[static_cast<size_t>(y) * stride + x]); };
    // Edge bits: horizontal edge (x, y) from (x, y) to (x + 1, y); vertical edge (x, y) from (x, y) to (x, y + 1)
    const size_t horizontalCount = static_cast<size_t>(width) * height;
    auto edgeBit = [&](int x, int y, int side) -> size_t {
        switch (side) {
            case TOP:
                return static_cast<size_t>(y) * width + x;
            case BOTTOM:
                return static_cast<size_t>(y + 1) * width + x;
            case LEFT:
                return horizontalCount + static_cast<size_t>(y) * width + x;
            default:
                return horizontalCount + static_cast<size_t>(y) * width + x + 1;
        }
    };
    auto isVisited = [&](size_t bit) { return (visited[bit >> 3] & (1u << (bit & 7))) != 0; };
    auto mark = [&](size_t bit) { visited[bit >> 3] = static_cast<uint8_t>(visited[bit >> 3] | (1u << (bit & 7))); };
    const float threshold = static_cast<float>(level);
    // The crossing on a side of a cell, interpolated between its two corners
    auto crossing = [&](int x, int y, int side, float& px, float& py) {
        int ax = x;
        int ay = y;
        int bx = x + 1;
        int by = y;
        if (side == BOTTOM) {
            ay = by = y + 1;
        } else if (side == LEFT) {
            bx = x;
            by = y + 1;
        } else if (side == RIGHT) {
            ax = bx = x + 1;
            by = y + 1;
        }
        const float va = value(ax, ay);
        const float vb = value(bx, by);
        const float t = vb != va ? std::clamp((threshold - va) / (vb - va), 0.0f, 1.0f) : 0.5f;
        px = static_cast<float>(ax) + (static_cast<float>(bx - ax)) * t;
        py = static_cast<float>(ay) + (static_cast<float>(by - ay)) * t;
    };
    const size_t startBit = edgeBit(cellX, cellY, entrySide);
    if (isVisited(startBit)) {
        return;
    }
    int count = 0;
    bool closed = false;
    auto add = [&](float px, float py) {
        if (count == maxPoints) {
            // A very long line goes out in pieces; the next piece starts at this piece's last point
            sink(context, levelM, points, count, false);
            lines++;
            points[0] = points[2 * (count - 1)];
            points[1] = points[2 * (count - 1) + 1];
            count = 1;
        }
        points[2 * count] = px;
        points[2 * count + 1] = py;
        count++;
    };
    float px;
    float py;
    crossing(cellX, cellY, entrySide, px, py);
    add(px, py);
    mark(startBit);
    int x = cellX;
    int y = cellY;
    int entry = entrySide;
    // A line can pass each edge once: this bounds the walk even on a damaged grid
    for (size_t steps = 0; steps < horizontalCount * 2; steps++) {
        const bool tl = value(x, y) >= threshold;
        const bool tr = value(x + 1, y) >= threshold;
        const bool br = value(x + 1, y + 1) >= threshold;
        const bool bl = value(x, y + 1) >= threshold;
        const bool crosses[4] = { tl != tr, tr != br, bl != br, tl != bl };
        int exitSide = -1;
        int crossingCount = 0;
        for (int side = 0; side < 4; side++) {
            crossingCount += crosses[side] ? 1 : 0;
        }
        if (crossingCount == 2) {
            for (int side = 0; side < 4 && exitSide < 0; side++) {
                if (crosses[side] && side != entry) {
                    exitSide = side;
                }
            }
        } else if (crossingCount == 4) {
            // A saddle: the centre says which two corners are joined through the cell
            const float centre = (value(x, y) + value(x + 1, y) + value(x + 1, y + 1) + value(x, y + 1)) * 0.25f;
            const bool joinedTopLeft = (centre >= threshold) == tl;
            // Joined top-left and bottom-right: the lines cut off the top-right and bottom-left corners
            static constexpr int PAIRED_CUT_CORNERS[4] = { RIGHT, TOP, LEFT, BOTTOM };
            static constexpr int PAIRED_OTHER[4] = { LEFT, BOTTOM, RIGHT, TOP };
            exitSide = joinedTopLeft ? PAIRED_CUT_CORNERS[entry] : PAIRED_OTHER[entry];
        }
        if (exitSide < 0) {
            break;
        }
        const size_t exitBit = edgeBit(x, y, exitSide);
        crossing(x, y, exitSide, px, py);
        add(px, py);
        if (isVisited(exitBit)) {
            closed = exitBit == startBit;
            break;
        }
        mark(exitBit);
        // Into the neighbour across that side
        if (exitSide == TOP) {
            y--;
        } else if (exitSide == BOTTOM) {
            y++;
        } else if (exitSide == LEFT) {
            x--;
        } else {
            x++;
        }
        entry = (exitSide + 2) % 4;
        if (x < 0 || y < 0 || x + 1 >= width || y + 1 >= height) {
            break;
        }
    }
    if (count >= 2) {
        sink(context, levelM, points, count, closed);
        lines++;
    }
}

int simplifyLine(float* points, int pointCount, float tolerance) {
    if (pointCount <= 2) {
        return pointCount;
    }
    // Douglas-Peucker without recursion: a stack of ranges and a keep flag per point, both in large memory
    auto* keep = static_cast<uint8_t*>(largeAlloc(static_cast<size_t>(pointCount)));
    auto* stack = static_cast<int*>(largeAlloc(static_cast<size_t>(pointCount) * 2 * sizeof(int)));
    if (keep == nullptr || stack == nullptr) {
        largeFree(keep);
        largeFree(stack);
        return pointCount;
    }
    memset(keep, 0, static_cast<size_t>(pointCount));
    keep[0] = 1;
    keep[pointCount - 1] = 1;
    int top = 0;
    stack[top++] = 0;
    stack[top++] = pointCount - 1;
    const float squaredTolerance = tolerance * tolerance;
    while (top > 0) {
        const int last = stack[--top];
        const int first = stack[--top];
        const float ax = points[2 * first];
        const float ay = points[2 * first + 1];
        const float dx = points[2 * last] - ax;
        const float dy = points[2 * last + 1] - ay;
        const float lengthSquared = dx * dx + dy * dy;
        float worst = -1.0f;
        int worstIndex = -1;
        for (int i = first + 1; i < last; i++) {
            const float qx = points[2 * i] - ax;
            const float qy = points[2 * i + 1] - ay;
            float distanceSquared;
            if (lengthSquared > 0.0f) {
                const float cross = qx * dy - qy * dx;
                distanceSquared = cross * cross / lengthSquared;
            } else {
                distanceSquared = qx * qx + qy * qy;
            }
            if (distanceSquared > worst) {
                worst = distanceSquared;
                worstIndex = i;
            }
        }
        if (worstIndex >= 0 && worst > squaredTolerance) {
            keep[worstIndex] = 1;
            stack[top++] = first;
            stack[top++] = worstIndex;
            stack[top++] = worstIndex;
            stack[top++] = last;
        }
    }
    int kept = 0;
    for (int i = 0; i < pointCount; i++) {
        if (keep[i] != 0) {
            points[2 * kept] = points[2 * i];
            points[2 * kept + 1] = points[2 * i + 1];
            kept++;
        }
    }
    largeFree(keep);
    largeFree(stack);
    return kept;
}

} // namespace maps
