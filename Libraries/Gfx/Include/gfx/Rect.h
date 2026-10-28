// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace gfx {

/** A pixel coordinate. */
struct Point {
    int x = 0;
    int y = 0;
};

/** A half-open pixel rectangle in world coordinates: x0/y0 inclusive, x1/y1 exclusive. */
struct Rect {
    int x0 = 0;
    int y0 = 0;
    int x1 = 0;
    int y1 = 0;

    int width() const { return x1 - x0; }
    int height() const { return y1 - y0; }
    bool empty() const { return x0 >= x1 || y0 >= y1; }

    bool contains(int x, int y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }

    /** The overlapping part, or an empty rectangle when they don't overlap. */
    Rect intersect(const Rect& other) const {
        const int nx0 = x0 > other.x0 ? x0 : other.x0;
        const int ny0 = y0 > other.y0 ? y0 : other.y0;
        const int nx1 = x1 < other.x1 ? x1 : other.x1;
        const int ny1 = y1 < other.y1 ? y1 : other.y1;
        if (nx0 >= nx1 || ny0 >= ny1) {
            return {};
        }
        return { nx0, ny0, nx1, ny1 };
    }

    /** The smallest rectangle covering both. An empty rectangle contributes nothing. */
    Rect unite(const Rect& other) const {
        if (empty()) {
            return other;
        }
        if (other.empty()) {
            return *this;
        }
        return {
            x0 < other.x0 ? x0 : other.x0,
            y0 < other.y0 ? y0 : other.y0,
            x1 > other.x1 ? x1 : other.x1,
            y1 > other.y1 ? y1 : other.y1,
        };
    }

    Rect offset(int dx, int dy) const { return { x0 + dx, y0 + dy, x1 + dx, y1 + dy }; }

    bool intersects(const Rect& other) const { return !intersect(other).empty(); }
};

} // namespace gfx
