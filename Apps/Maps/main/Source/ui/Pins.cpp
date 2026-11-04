// SPDX-License-Identifier: GPL-3.0-or-later
#include "Pins.h"

namespace maps {

void drawPin(Canvas& canvas, float x, float y, const PinStyle& style, bool highlighted) {
    const float headY = y - PIN_HEAD_OFFSET;
    canvas.line(x, y, x, headY, 2.0f, style.stem);
    canvas.fillCircle(x, headY, PIN_RADIUS + 1.5f, style.halo);
    canvas.fillCircle(x, headY, PIN_RADIUS, highlighted ? style.highlight : style.fill);
}

} // namespace maps
