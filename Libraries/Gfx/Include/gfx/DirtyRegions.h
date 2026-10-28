// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Rect.h"

namespace gfx {

/**
 * A small set of changed rectangles.
 *
 * New rectangles are merged into every rectangle they intersect, so a frame that clears an object's
 * bounding box and then draws primitives inside it collapses back into that single rectangle
 * instead of one entry per primitive. When the set is full, further rectangles are merged into the
 * last entry.
 */
class DirtyRegions {
public:
    static constexpr int CAPACITY = 8;

    void clear() { count_ = 0; }

    int count() const { return count_; }

    Rect at(int index) const { return rects_[index]; }

    void add(Rect rect) {
        if (rect.empty()) {
            return;
        }

        // Merge with every rectangle this one touches, then re-check the merged result: growing it
        // can make it overlap entries that the original didn't.
        int i = 0;
        while (i < count_) {
            if (rects_[i].intersects(rect)) {
                rect = rects_[i].unite(rect);
                rects_[i] = rects_[--count_];
                i = 0;
            } else {
                i++;
            }
        }

        if (count_ < CAPACITY) {
            rects_[count_++] = rect;
        } else {
            rects_[CAPACITY - 1] = rects_[CAPACITY - 1].unite(rect);
        }
    }

private:
    Rect rects_[CAPACITY] {};
    int count_ = 0;
};

} // namespace gfx
