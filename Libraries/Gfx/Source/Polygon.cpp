// SPDX-License-Identifier: Apache-2.0
#include <gfx/Surface.h>

#include <algorithm>

namespace gfx {

namespace {

// Local rounding avoids lroundf, which the current firmware doesn't export to app ELFs.
// Replace with std::lround()/lroundf() once the c-symbols module exports it (see DESIGN.md,
// "Symbol export notes").
int roundToInt(float value) {
    return static_cast<int>(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

} // namespace

void Surface::drawPolygon(const Point* points, int count, uint16_t rgb565) {
    if (points == nullptr || count < 2 || count > MAX_POLYGON_POINTS) {
        return;
    }

    int minX = points[0].x;
    int maxX = points[0].x;
    int minY = points[0].y;
    int maxY = points[0].y;
    for (int i = 1; i < count; i++) {
        minX = std::min(minX, points[i].x);
        maxX = std::max(maxX, points[i].x);
        minY = std::min(minY, points[i].y);
        maxY = std::max(maxY, points[i].y);
    }

    const Rect box = Rect { minX, minY, maxX + 1, maxY + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    for (int i = 0; i < count; i++) {
        const Point& a = points[i];
        const Point& b = points[(i + 1) % count];
        lineRaw(a.x, a.y, b.x, b.y, rgb565);
    }
}

void Surface::fillPolygon(const Point* points, int count, uint16_t rgb565) {
    if (points == nullptr || count < 3 || count > MAX_POLYGON_POINTS) {
        return;
    }

    int minX = points[0].x;
    int maxX = points[0].x;
    int minY = points[0].y;
    int maxY = points[0].y;
    for (int i = 1; i < count; i++) {
        minX = std::min(minX, points[i].x);
        maxX = std::max(maxX, points[i].x);
        minY = std::min(minY, points[i].y);
        maxY = std::max(maxY, points[i].y);
    }

    const Rect box = Rect { minX, minY, maxX + 1, maxY + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    const int startY = std::max(minY, clip_.y0);
    const int endY = std::min(maxY, clip_.y1 - 1);
    int intersections[MAX_POLYGON_POINTS];

    for (int y = startY; y <= endY; y++) {
        // Sample the middle of the row so vertices don't count twice.
        const float scanY = static_cast<float>(y) + 0.5f;
        int intersectionCount = 0;

        for (int i = 0; i < count; i++) {
            const Point& a = points[i];
            const Point& b = points[(i + 1) % count];
            if (a.y == b.y) {
                continue;
            }

            const int lowY = std::min(a.y, b.y);
            const int highY = std::max(a.y, b.y);
            if (scanY < lowY || scanY >= highY) {
                continue;
            }

            const float t = (scanY - static_cast<float>(a.y)) / static_cast<float>(b.y - a.y);
            intersections[intersectionCount++] = roundToInt(static_cast<float>(a.x) + t * static_cast<float>(b.x - a.x));
        }

        std::sort(intersections, intersections + intersectionCount);
        for (int i = 0; i + 1 < intersectionCount; i += 2) {
            const int x0 = intersections[i];
            const int x1 = intersections[i + 1];
            if (x0 < x1) {
                fillSpanRaw(x0, x1, y, rgb565);
            }
        }
    }
}

} // namespace gfx
