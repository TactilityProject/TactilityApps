// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Canvas.h"

#include <cstdint>

namespace maps {

/** How waypoint pins look: a round head on a short stem, the point at the stem's foot. */
struct PinStyle {
    uint16_t fill;
    /** The pin whose sheet is open. Same size as the others, only the colour differs. */
    uint16_t highlight;
    uint16_t halo;
    uint16_t stem;
};

constexpr float PIN_RADIUS = 5.0f;
/** How far above its point a pin's head is drawn. */
constexpr float PIN_HEAD_OFFSET = 2.0f * PIN_RADIUS;

/**
 * Draws one pin with its point at x, y. Every pin has the same size at every zoom level and in every state: the size
 * depends on nothing but PIN_RADIUS.
 */
void drawPin(Canvas& canvas, float x, float y, const PinStyle& style, bool highlighted);

} // namespace maps
