// SPDX-License-Identifier: GPL-3.0-or-later
#include "MenuIcons.h"

#include <algorithm>
#include <cmath>

namespace maps {

namespace {

/**
 * A pen that draws in a 24 unit grid around the icon's centre, scaled to the real size. Keeping the shapes in grid
 * units lets one drawing serve every size.
 */
struct Pen {
    Canvas& canvas;
    float cx;
    float cy;
    float scale;
    float stroke;
    uint16_t color;

    float x(float gx) const { return cx + gx * scale; }
    float y(float gy) const { return cy + gy * scale; }

    void line(float x0, float y0, float x1, float y1, float widthFactor = 1.0f) const {
        canvas.line(x(x0), y(y0), x(x1), y(y1), stroke * widthFactor, color);
    }
    void ring(float gx, float gy, float radius) const {
        canvas.ring(x(gx), y(gy), radius * scale, stroke, color);
    }
    void dot(float gx, float gy, float radius) const {
        canvas.fillCircle(x(gx), y(gy), std::max(radius * scale, stroke * 0.6f), color);
    }
    /** A line that stops short of both ends, to leave room for a ring round a node. */
    void trimmed(float x0, float y0, float x1, float y1, float gapStart, float gapEnd) const {
        const float dx = x1 - x0;
        const float dy = y1 - y0;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length <= gapStart + gapEnd) {
            return;
        }
        const float ux = dx / length;
        const float uy = dy / length;
        line(x0 + ux * gapStart, y0 + uy * gapStart, x1 - ux * gapEnd, y1 - uy * gapEnd);
    }
};

int roundToInt(float value) {
    return static_cast<int>(std::floor(value + 0.5f));
}

} // namespace

void drawMenuIcon(Canvas& canvas, MenuIcon icon, int centreX, int centreY, int size, uint16_t color) {
    const float scale = static_cast<float>(size) / 24.0f;
    const Pen pen { canvas, static_cast<float>(centreX), static_cast<float>(centreY), scale, std::max(1.5f, static_cast<float>(size) / 12.0f), color };
    switch (icon) {
        case MenuIcon::None:
            break;
        case MenuIcon::Search:
            pen.ring(-2.5f, -2.5f, 7.0f);
            pen.line(2.8f, 2.8f, 10.0f, 10.0f, 1.3f);
            break;
        case MenuIcon::Download:
            pen.line(0.0f, -10.0f, 0.0f, 4.0f);
            pen.line(-5.5f, -1.5f, 0.0f, 4.5f);
            pen.line(5.5f, -1.5f, 0.0f, 4.5f);
            // The tray the file lands in
            pen.line(-9.0f, 6.0f, -9.0f, 10.0f);
            pen.line(-9.0f, 10.0f, 9.0f, 10.0f);
            pen.line(9.0f, 10.0f, 9.0f, 6.0f);
            break;
        case MenuIcon::Maps:
            // A folded sheet: zigzag top and bottom, a fold line between the panels
            pen.line(-10.0f, -6.0f, -3.5f, -9.0f);
            pen.line(-3.5f, -9.0f, 3.5f, -6.0f);
            pen.line(3.5f, -6.0f, 10.0f, -9.0f);
            pen.line(-10.0f, 9.0f, -3.5f, 6.0f);
            pen.line(-3.5f, 6.0f, 3.5f, 9.0f);
            pen.line(3.5f, 9.0f, 10.0f, 6.0f);
            pen.line(-10.0f, -6.0f, -10.0f, 9.0f);
            pen.line(-3.5f, -9.0f, -3.5f, 6.0f);
            pen.line(3.5f, -6.0f, 3.5f, 9.0f);
            pen.line(10.0f, -9.0f, 10.0f, 6.0f);
            break;
        case MenuIcon::Waypoints:
            pen.ring(0.0f, -4.0f, 6.0f);
            pen.line(-5.2f, -1.0f, 0.0f, 10.5f);
            pen.line(5.2f, -1.0f, 0.0f, 10.5f);
            pen.dot(0.0f, -4.0f, 2.0f);
            break;
        case MenuIcon::Track: {
            // Dots along a bend, ending in a bigger one: where the recording is now
            const float dots[][2] = { { -9.0f, 9.0f }, { -6.5f, 4.0f }, { -2.5f, 1.0f }, { 2.5f, -1.0f }, { 5.0f, -5.0f } };
            for (const auto& d : dots) {
                pen.dot(d[0], d[1], 1.6f);
            }
            pen.dot(8.5f, -9.0f, 3.4f);
            break;
        }
        case MenuIcon::Gpx: {
            const float nodes[][2] = { { -8.0f, 7.0f }, { -1.5f, -5.0f }, { 8.0f, 4.0f } };
            const float gap = 3.6f;
            pen.trimmed(nodes[0][0], nodes[0][1], nodes[1][0], nodes[1][1], gap, gap);
            pen.trimmed(nodes[1][0], nodes[1][1], nodes[2][0], nodes[2][1], gap, gap);
            for (const auto& n : nodes) {
                pen.ring(n[0], n[1], 2.6f);
            }
            break;
        }
        case MenuIcon::Location:
            pen.ring(0.0f, 0.0f, 6.0f);
            pen.line(0.0f, -11.0f, 0.0f, -7.5f);
            pen.line(0.0f, 7.5f, 0.0f, 11.0f);
            pen.line(-11.0f, 0.0f, -7.5f, 0.0f);
            pen.line(7.5f, 0.0f, 11.0f, 0.0f);
            pen.dot(0.0f, 0.0f, 2.2f);
            break;
        case MenuIcon::Settings: {
            const float rows[] = { -7.0f, 0.0f, 7.0f };
            const float knobs[] = { -4.0f, 4.0f, -1.0f };
            for (int i = 0; i < 3; i++) {
                // The slider line leaves a gap for the knob
                pen.line(-10.0f, rows[i], knobs[i] - 3.8f, rows[i]);
                pen.line(knobs[i] + 3.8f, rows[i], 10.0f, rows[i]);
                pen.ring(knobs[i], rows[i], 2.6f);
            }
            break;
        }
        case MenuIcon::About:
            pen.ring(0.0f, 0.0f, 10.0f);
            pen.dot(0.0f, -4.8f, 1.5f);
            pen.line(0.0f, -1.2f, 0.0f, 5.5f, 1.2f);
            break;
        case MenuIcon::Exit:
            pen.line(-1.0f, -9.0f, -9.0f, -9.0f);
            pen.line(-9.0f, -9.0f, -9.0f, 9.0f);
            pen.line(-9.0f, 9.0f, -1.0f, 9.0f);
            pen.line(-3.0f, 0.0f, 10.0f, 0.0f);
            pen.line(5.0f, -5.0f, 10.0f, 0.0f);
            pen.line(5.0f, 5.0f, 10.0f, 0.0f);
            break;
        case MenuIcon::Route:
            pen.ring(-8.0f, 8.0f, 2.8f);
            pen.line(-8.0f, 5.0f, -8.0f, -1.0f);
            pen.line(-8.0f, -1.0f, 8.0f, -1.0f);
            pen.line(8.0f, -1.0f, 8.0f, -5.0f);
            pen.dot(8.0f, -8.0f, 3.2f);
            break;
        case MenuIcon::Stop: {
            const int side = std::max(8, roundToInt(14.0f * scale));
            canvas.fillRoundRect(centreX - side / 2, centreY - side / 2, side, side, 1, color);
            break;
        }
        case MenuIcon::Clear:
            pen.line(-8.0f, -8.0f, 8.0f, 8.0f, 1.2f);
            pen.line(-8.0f, 8.0f, 8.0f, -8.0f, 1.2f);
            break;
        case MenuIcon::Pause: {
            const int bar = std::max(3, roundToInt(5.0f * scale));
            const int tall = std::max(8, roundToInt(16.0f * scale));
            const int gap = std::max(3, roundToInt(4.0f * scale));
            canvas.fillRoundRect(centreX - gap / 2 - bar, centreY - tall / 2, bar, tall, 1, color);
            canvas.fillRoundRect(centreX + (gap + 1) / 2, centreY - tall / 2, bar, tall, 1, color);
            break;
        }
    }
}

} // namespace maps
