// SPDX-License-Identifier: Apache-2.0
#include <gfx/Surface.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace gfx {

namespace {

constexpr double DEGREES_TO_RADIANS = 3.14159265358979323846 / 180.0;

/** Absorbs the rounding error of sin/cos when a point lies exactly on an arc boundary. */
constexpr double ARC_ANGLE_EPSILON = 1e-6;

float normaliseDegrees(float degrees) {
    degrees = std::fmod(degrees, 360.0f);
    if (degrees < 0.0f) {
        degrees += 360.0f;
    }
    return degrees;
}

/** Rounds a value to the nearest integer, for arc endpoints. */
int roundToInt(float value) {
    return static_cast<int>(value >= 0.0f ? value + 0.5f : value - 0.5f);
}

} // namespace

void Surface::fillCircle(int cx, int cy, int radius, uint16_t rgb565) {
    if (radius < 0) {
        return;
    }

    const Rect box = Rect { cx - radius, cy - radius, cx + radius + 1, cy + radius + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    if (radius == 0) {
        fillSpanRaw(cx, cx + 1, cy, rgb565);
        return;
    }

    const int64_t radius_squared = static_cast<int64_t>(radius) * radius;
    int x = radius;
    for (int y = 0; y <= radius; y++) {
        while (x > 0 && static_cast<int64_t>(x) * x + static_cast<int64_t>(y) * y > radius_squared) {
            x--;
        }
        fillSpanRaw(cx - x, cx + x + 1, cy + y, rgb565);
        if (y != 0) {
            fillSpanRaw(cx - x, cx + x + 1, cy - y, rgb565);
        }
    }
}

void Surface::drawCircle(int cx, int cy, int radius, uint16_t rgb565) {
    if (radius < 0) {
        return;
    }

    const Rect box = Rect { cx - radius, cy - radius, cx + radius + 1, cy + radius + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    if (radius == 0) {
        plotRaw(cx, cy, rgb565);
        return;
    }

    auto plotOctants = [&](int x, int y) {
        plotRaw(cx + x, cy + y, rgb565);
        plotRaw(cx - x, cy + y, rgb565);
        plotRaw(cx + x, cy - y, rgb565);
        plotRaw(cx - x, cy - y, rgb565);
        plotRaw(cx + y, cy + x, rgb565);
        plotRaw(cx - y, cy + x, rgb565);
        plotRaw(cx + y, cy - x, rgb565);
        plotRaw(cx - y, cy - x, rgb565);
    };

    int x = radius;
    int y = 0;
    int error = 1 - radius;
    while (x >= y) {
        plotOctants(x, y);
        y++;
        if (error < 0) {
            error += 2 * y + 1;
        } else {
            x--;
            error += 2 * (y - x) + 1;
        }
    }
}

void Surface::fillEllipse(int cx, int cy, int radiusX, int radiusY, uint16_t rgb565) {
    if (radiusX < 0 || radiusY < 0) {
        return;
    }

    const Rect box = Rect { cx - radiusX, cy - radiusY, cx + radiusX + 1, cy + radiusY + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    if (radiusX == 0 && radiusY == 0) {
        plotRaw(cx, cy, rgb565);
        return;
    }
    if (radiusX == 0) {
        fillRectRaw(Rect { cx, cy - radiusY, cx + 1, cy + radiusY + 1 }, rgb565);
        return;
    }
    if (radiusY == 0) {
        fillSpanRaw(cx - radiusX, cx + radiusX + 1, cy, rgb565);
        return;
    }

    const int64_t rx2 = static_cast<int64_t>(radiusX) * radiusX;
    const int64_t ry2 = static_cast<int64_t>(radiusY) * radiusY;
    const int64_t limit = rx2 * ry2;

    int x = radiusX;
    for (int y = 0; y <= radiusY; y++) {
        while (x > 0 && static_cast<int64_t>(x) * x * ry2 + static_cast<int64_t>(y) * y * rx2 > limit) {
            x--;
        }
        fillSpanRaw(cx - x, cx + x + 1, cy + y, rgb565);
        if (y != 0) {
            fillSpanRaw(cx - x, cx + x + 1, cy - y, rgb565);
        }
    }
}

void Surface::drawEllipse(int cx, int cy, int radiusX, int radiusY, uint16_t rgb565) {
    if (radiusX < 0 || radiusY < 0) {
        return;
    }

    const Rect box = Rect { cx - radiusX, cy - radiusY, cx + radiusX + 1, cy + radiusY + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    if (radiusX == 0 && radiusY == 0) {
        plotRaw(cx, cy, rgb565);
        return;
    }
    if (radiusX == 0) {
        lineRaw(cx, cy - radiusY, cx, cy + radiusY, rgb565);
        return;
    }
    if (radiusY == 0) {
        lineRaw(cx - radiusX, cy, cx + radiusX, cy, rgb565);
        return;
    }

    const int64_t rx2 = static_cast<int64_t>(radiusX) * radiusX;
    const int64_t ry2 = static_cast<int64_t>(radiusY) * radiusY;
    const int64_t limit = rx2 * ry2;

    auto plotQuadrants = [&](int x, int y) {
        plotRaw(cx + x, cy + y, rgb565);
        plotRaw(cx - x, cy + y, rgb565);
        plotRaw(cx + x, cy - y, rgb565);
        plotRaw(cx - x, cy - y, rgb565);
    };

    // Walk the curve both as x(y) and as y(x). The union covers the nearly-horizontal and
    // nearly-vertical parts, which either walk on its own would leave with gaps.
    int x = radiusX;
    for (int y = 0; y <= radiusY; y++) {
        while (x > 0 && static_cast<int64_t>(x) * x * ry2 + static_cast<int64_t>(y) * y * rx2 > limit) {
            x--;
        }
        plotQuadrants(x, y);
    }

    int y = radiusY;
    for (int sample_x = 0; sample_x <= radiusX; sample_x++) {
        while (y > 0 && static_cast<int64_t>(sample_x) * sample_x * ry2 + static_cast<int64_t>(y) * y * rx2 > limit) {
            y--;
        }
        plotQuadrants(sample_x, y);
    }
}

void Surface::fillRoundRect(int x, int y, int width, int height, int radius, uint16_t rgb565) {
    if (width <= 0 || height <= 0) {
        return;
    }
    if (radius <= 0) {
        fillRect(x, y, width, height, rgb565);
        return;
    }

    radius = std::min(radius, std::min(width, height) / 2);
    if (radius <= 0) {
        fillRect(x, y, width, height, rgb565);
        return;
    }

    const Rect box = Rect { x, y, x + width, y + height }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    // The centre rectangle and the two straight strips between the corners.
    fillRectRaw(Rect { x, y + radius, x + width, y + height - radius }, rgb565);
    fillRectRaw(Rect { x + radius, y, x + width - radius, y + radius }, rgb565);
    fillRectRaw(Rect { x + radius, y + height - radius, x + width - radius, y + height }, rgb565);

    const int64_t radius_squared = static_cast<int64_t>(radius) * radius;
    int dx = 0;
    for (int row = 0; row < radius; row++) {
        const int dy = radius - row;
        while (dx < radius && static_cast<int64_t>(dx + 1) * (dx + 1) + static_cast<int64_t>(dy) * dy <= radius_squared) {
            dx++;
        }
        const int inset = radius - dx;
        fillSpanRaw(x + inset, x + width - inset, y + row, rgb565);
        fillSpanRaw(x + inset, x + width - inset, y + height - 1 - row, rgb565);
    }
}

void Surface::drawRoundRect(int x, int y, int width, int height, int radius, uint16_t rgb565) {
    if (width <= 0 || height <= 0) {
        return;
    }
    if (radius <= 0) {
        drawRect(x, y, width, height, rgb565);
        return;
    }

    radius = std::min(radius, std::min(width, height) / 2);
    if (radius <= 0) {
        drawRect(x, y, width, height, rgb565);
        return;
    }

    const Rect box = Rect { x, y, x + width, y + height }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    fillSpanRaw(x + radius, x + width - radius, y, rgb565);
    fillSpanRaw(x + radius, x + width - radius, y + height - 1, rgb565);
    fillRectRaw(Rect { x, y + radius, x + 1, y + height - radius }, rgb565);
    fillRectRaw(Rect { x + width - 1, y + radius, x + width, y + height - radius }, rgb565);

    const int64_t radius_squared = static_cast<int64_t>(radius) * radius;
    int dx = 0;
    for (int row = 0; row < radius; row++) {
        const int dy = radius - row;
        while (dx < radius && static_cast<int64_t>(dx + 1) * (dx + 1) + static_cast<int64_t>(dy) * dy <= radius_squared) {
            dx++;
        }
        const int inset = radius - dx;
        plotRaw(x + inset, y + row, rgb565);
        plotRaw(x + width - 1 - inset, y + row, rgb565);
        plotRaw(x + inset, y + height - 1 - row, rgb565);
        plotRaw(x + width - 1 - inset, y + height - 1 - row, rgb565);
    }
}

void Surface::fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t rgb565) {
    // Sort the vertices by y so the scanline fill can run top to bottom.
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }
    if (y1 > y2) {
        std::swap(x1, x2);
        std::swap(y1, y2);
    }
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
    }

    const int min_x = std::min({ x0, x1, x2 });
    const int max_x = std::max({ x0, x1, x2 });
    const Rect box = Rect { min_x, y0, max_x + 1, y2 + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    if (y0 == y2) {
        fillSpanRaw(min_x, max_x + 1, y0, rgb565);
        return;
    }

    // Fixed-point slopes (16 subpixel bits) keep the edges stable without floating point.
    const int32_t dx02 = (static_cast<int32_t>(x2 - x0) << 4) / (y2 - y0);
    int32_t xa = static_cast<int32_t>(x0) << 4;

    if (y1 == y0) {
        const int32_t dx12 = (static_cast<int32_t>(x2 - x1) << 4) / (y2 - y1);
        int32_t xb = static_cast<int32_t>(x1) << 4;
        for (int y = y0; y <= y2; y++) {
            int low = static_cast<int>(xa >> 4);
            int high = static_cast<int>(xb >> 4);
            if (low > high) {
                std::swap(low, high);
            }
            fillSpanRaw(low, high + 1, y, rgb565);
            xa += dx02;
            xb += dx12;
        }
        return;
    }

    if (y1 == y2) {
        const int32_t dx01 = (static_cast<int32_t>(x1 - x0) << 4) / (y1 - y0);
        int32_t xb = static_cast<int32_t>(x0) << 4;
        for (int y = y0; y <= y1; y++) {
            int low = static_cast<int>(xa >> 4);
            int high = static_cast<int>(xb >> 4);
            if (low > high) {
                std::swap(low, high);
            }
            fillSpanRaw(low, high + 1, y, rgb565);
            xa += dx02;
            xb += dx01;
        }
        return;
    }

    const int32_t dx01 = (static_cast<int32_t>(x1 - x0) << 4) / (y1 - y0);
    int32_t xb = static_cast<int32_t>(x0) << 4;
    for (int y = y0; y < y1; y++) {
        int low = static_cast<int>(xa >> 4);
        int high = static_cast<int>(xb >> 4);
        if (low > high) {
            std::swap(low, high);
        }
        fillSpanRaw(low, high + 1, y, rgb565);
        xa += dx02;
        xb += dx01;
    }

    const int32_t dx12 = (static_cast<int32_t>(x2 - x1) << 4) / (y2 - y1);
    xb = static_cast<int32_t>(x1) << 4;
    for (int y = y1; y <= y2; y++) {
        int low = static_cast<int>(xa >> 4);
        int high = static_cast<int>(xb >> 4);
        if (low > high) {
            std::swap(low, high);
        }
        fillSpanRaw(low, high + 1, y, rgb565);
        xa += dx02;
        xb += dx12;
    }
}

void Surface::drawTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t rgb565) {
    const int min_x = std::min({ x0, x1, x2 });
    const int max_x = std::max({ x0, x1, x2 });
    const int min_y = std::min({ y0, y1, y2 });
    const int max_y = std::max({ y0, y1, y2 });
    const Rect box = Rect { min_x, min_y, max_x + 1, max_y + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    lineRaw(x0, y0, x1, y1, rgb565);
    lineRaw(x1, y1, x2, y2, rgb565);
    lineRaw(x2, y2, x0, y0, rgb565);
}

void Surface::fillArc(int cx, int cy, int outerRadius, int innerRadius, float startDegrees, float endDegrees, uint16_t rgb565) {
    if (outerRadius <= 0) {
        return;
    }
    if (innerRadius < 0) {
        innerRadius = 0;
    }
    if (innerRadius > outerRadius) {
        std::swap(outerRadius, innerRadius);
    }

    const Rect box = Rect { cx - outerRadius, cy - outerRadius, cx + outerRadius + 1, cy + outerRadius + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    const float start = normaliseDegrees(startDegrees);
    const float end = normaliseDegrees(endDegrees);
    float sweep = end - start;
    if (sweep <= 0.0f) {
        sweep += 360.0f;
    }
    const bool full = sweep >= 359.999f;
    const bool wide = sweep > 180.0f;

    const double start_radians = start * DEGREES_TO_RADIANS;
    const double end_radians = end * DEGREES_TO_RADIANS;
    const double start_sin = std::sin(start_radians);
    const double start_cos = std::cos(start_radians);
    const double end_sin = std::sin(end_radians);
    const double end_cos = std::cos(end_radians);

    const int64_t outer_squared = static_cast<int64_t>(outerRadius) * outerRadius;
    const int64_t inner_squared = static_cast<int64_t>(innerRadius) * innerRadius;

    for (int y = box.y0; y < box.y1; y++) {
        const int dy = y - cy;
        for (int x = box.x0; x < box.x1; x++) {
            const int dx = x - cx;
            const int64_t distance_squared = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
            if (distance_squared > outer_squared || distance_squared < inner_squared) {
                continue;
            }

            if (!full) {
                const double cross_start = start_cos * dy - start_sin * dx;
                const double cross_end = dx * end_sin - dy * end_cos;
                const bool inside = wide ? (cross_start >= -ARC_ANGLE_EPSILON || cross_end >= -ARC_ANGLE_EPSILON) : (cross_start >= -ARC_ANGLE_EPSILON && cross_end >= -ARC_ANGLE_EPSILON);
                if (!inside) {
                    continue;
                }
            }

            plotRaw(x, y, rgb565);
        }
    }
}

void Surface::drawArc(int cx, int cy, int outerRadius, int innerRadius, float startDegrees, float endDegrees, uint16_t rgb565) {
    if (outerRadius <= 0) {
        return;
    }
    if (innerRadius < 0) {
        innerRadius = 0;
    }
    if (innerRadius > outerRadius) {
        std::swap(outerRadius, innerRadius);
    }

    const Rect box = Rect { cx - outerRadius, cy - outerRadius, cx + outerRadius + 1, cy + outerRadius + 1 }.intersect(clip_);
    if (box.empty()) {
        return;
    }
    markDirtyRect(box.x0, box.y0, box.x1, box.y1);

    const float start = normaliseDegrees(startDegrees);
    const float end = normaliseDegrees(endDegrees);
    float sweep = end - start;
    if (sweep <= 0.0f) {
        sweep += 360.0f;
    }
    const bool full = sweep >= 359.999f;
    const bool wide = sweep > 180.0f;

    const double start_radians = start * DEGREES_TO_RADIANS;
    const double end_radians = end * DEGREES_TO_RADIANS;
    const double start_sin = std::sin(start_radians);
    const double start_cos = std::cos(start_radians);
    const double end_sin = std::sin(end_radians);
    const double end_cos = std::cos(end_radians);

    const int64_t outer_squared = static_cast<int64_t>(outerRadius) * outerRadius;
    const int64_t inner_outer_squared = static_cast<int64_t>(outerRadius - 1) * (outerRadius - 1);
    const int64_t inner_squared = static_cast<int64_t>(innerRadius) * innerRadius;
    const int64_t inner_ring_squared = static_cast<int64_t>(innerRadius + 1) * (innerRadius + 1);

    for (int y = box.y0; y < box.y1; y++) {
        const int dy = y - cy;
        for (int x = box.x0; x < box.x1; x++) {
            const int dx = x - cx;
            const int64_t distance_squared = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;

            const bool on_outer = distance_squared <= outer_squared && distance_squared > inner_outer_squared;
            const bool on_inner = innerRadius > 0 && distance_squared >= inner_squared && distance_squared < inner_ring_squared;
            if (!on_outer && !on_inner) {
                continue;
            }

            if (!full) {
                const double cross_start = start_cos * dy - start_sin * dx;
                const double cross_end = dx * end_sin - dy * end_cos;
                const bool inside = wide ? (cross_start >= -ARC_ANGLE_EPSILON || cross_end >= -ARC_ANGLE_EPSILON) : (cross_start >= -ARC_ANGLE_EPSILON && cross_end >= -ARC_ANGLE_EPSILON);
                if (!inside) {
                    continue;
                }
            }

            plotRaw(x, y, rgb565);
        }
    }

    if (!full) {
        // Cap the two radial ends so an annulus reads as a wedge rather than two floating rings.
        const int outer_start_x = cx + roundToInt(static_cast<float>(outerRadius * start_cos));
        const int outer_start_y = cy + roundToInt(static_cast<float>(outerRadius * start_sin));
        const int inner_start_x = cx + roundToInt(static_cast<float>(innerRadius * start_cos));
        const int inner_start_y = cy + roundToInt(static_cast<float>(innerRadius * start_sin));
        const int outer_end_x = cx + roundToInt(static_cast<float>(outerRadius * end_cos));
        const int outer_end_y = cy + roundToInt(static_cast<float>(outerRadius * end_sin));
        const int inner_end_x = cx + roundToInt(static_cast<float>(innerRadius * end_cos));
        const int inner_end_y = cy + roundToInt(static_cast<float>(innerRadius * end_sin));

        lineRaw(outer_start_x, outer_start_y, inner_start_x, inner_start_y, rgb565);
        lineRaw(outer_end_x, outer_end_y, inner_end_x, inner_end_y, rgb565);
    }
}

} // namespace gfx
