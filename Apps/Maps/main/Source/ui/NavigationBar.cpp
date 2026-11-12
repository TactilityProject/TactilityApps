// SPDX-License-Identifier: GPL-3.0-or-later
#include "NavigationBar.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace maps {

namespace {

constexpr int PADDING = 8;
constexpr float DEGREES_TO_RADIANS = 3.14159265f / 180.0f;
constexpr float ARROW_WIDTH = 3.0f;

/** The direction angle degrees clockwise from straight up, as a step on the screen. */
void direction(float degrees, float& dx, float& dy) {
    const float radians = degrees * DEGREES_TO_RADIANS;
    dx = std::sin(radians);
    dy = -std::cos(radians);
}

/** A line from x, y by length in a direction, with a head of two short lines at its end. */
void arrowSegment(Canvas& canvas, float x, float y, float degrees, float length, float head, uint16_t color) {
    float dx;
    float dy;
    direction(degrees, dx, dy);
    const float tipX = x + dx * length;
    const float tipY = y + dy * length;
    canvas.line(x, y, tipX, tipY, ARROW_WIDTH, color);
    for (const float side : { -40.0f, 40.0f }) {
        float wx;
        float wy;
        direction(degrees + 180.0f + side, wx, wy);
        canvas.line(tipX, tipY, tipX + wx * head, tipY + wy * head, ARROW_WIDTH, color);
    }
}

/** The angle a turn bends the way, clockwise from straight on; Straight and the others are 0. */
float bendOf(Turn turn) {
    switch (turn) {
        case Turn::SlightLeft:
            return -30.0f;
        case Turn::Left:
            return -90.0f;
        case Turn::SharpLeft:
            return -135.0f;
        case Turn::SlightRight:
            return 30.0f;
        case Turn::Right:
            return 90.0f;
        case Turn::SharpRight:
            return 135.0f;
        default:
            return 0.0f;
    }
}

} // namespace

void NavigationBar::set(Turn turn, const char* instruction, const char* remaining) {
    nextTurn = turn;
    messageShown = false;
    snprintf(instructionText, sizeof(instructionText), "%s", instruction != nullptr ? instruction : "");
    snprintf(remainingText, sizeof(remainingText), "%s", remaining != nullptr ? remaining : "");
}

void NavigationBar::setMessage(const char* message) {
    messageShown = true;
    snprintf(instructionText, sizeof(instructionText), "%s", message != nullptr ? message : "");
}

bool NavigationBar::stopHit(int x, int y) const {
    return stopShown && stopArea.width > 0 && x >= stopArea.x - 6 && x < stopArea.x + stopArea.width + 6 && y >= 0 &&
        y < stopArea.y + stopArea.height + 6;
}

int NavigationBar::height(Font& font) const {
    return 2 * font.lineHeight() + PADDING + 4;
}

void NavigationBar::drawArrow(Canvas& canvas, const UiTheme& theme, const Rect& area) const {
    const auto cx = static_cast<float>(area.x) + static_cast<float>(area.width) / 2.0f;
    const auto cy = static_cast<float>(area.y) + static_cast<float>(area.height) / 2.0f;
    const float r = static_cast<float>(area.width) * 0.34f;
    const uint16_t color = theme.accentText;
    switch (nextTurn) {
        case Turn::Arrive:
            canvas.fillCircle(cx, cy, r * 0.5f, color);
            canvas.ring(cx, cy, r * 0.95f, 2.5f, color);
            break;
        case Turn::Roundabout: {
            const float ringRadius = r * 0.45f;
            const float ringY = cy + r * 0.05f;
            canvas.ring(cx, ringY, ringRadius, 2.5f, color);
            canvas.line(cx, cy + r, cx, ringY + ringRadius, ARROW_WIDTH, color);
            float dx;
            float dy;
            direction(45.0f, dx, dy);
            arrowSegment(canvas, cx + dx * ringRadius, ringY + dy * ringRadius, 45.0f, r * 0.75f, r * 0.55f, color);
            break;
        }
        case Turn::UTurn: {
            // Up the right side, over the top and down the left side, with the head pointing down
            const float lane = r * 0.4f;
            const float topY = cy - r * 0.4f;
            canvas.line(cx + lane, cy + r, cx + lane, topY, ARROW_WIDTH, color);
            float previousX = cx + lane;
            float previousY = topY;
            for (int step = 1; step <= 6; step++) {
                const float angle = static_cast<float>(step) * 30.0f * DEGREES_TO_RADIANS;
                const float x = cx + lane * std::cos(angle);
                const float y = topY - lane * std::sin(angle);
                canvas.line(previousX, previousY, x, y, ARROW_WIDTH, color);
                previousX = x;
                previousY = y;
            }
            arrowSegment(canvas, cx - lane, topY, 180.0f, r * 1.3f, r * 0.55f, color);
            break;
        }
        case Turn::Straight:
            arrowSegment(canvas, cx, cy + r, 0.0f, r * 2.0f, r * 0.6f, color);
            break;
        default: {
            // A stem up to the bend, then the way out at the turn's angle
            const float bend = bendOf(nextTurn);
            const float bendY = cy + r * 0.2f;
            canvas.line(cx, cy + r, cx, bendY, ARROW_WIDTH, color);
            arrowSegment(canvas, cx, bendY, bend, r * 1.1f, r * 0.6f, color);
            break;
        }
    }
}

void NavigationBar::draw(Canvas& canvas, Font& font, const UiTheme& theme, int width) {
    const int line = font.lineHeight();
    const int barHeight = height(font);
    canvas.fillRect(0, 0, width, barHeight, theme.panel, theme.panelOpacity);
    canvas.fillRect(0, barHeight - 1, width, 1, theme.border);
    // The arrow sits on an accent square, which keeps it readable on any map and on monochrome themes
    const Rect icon = { 4, 4, barHeight - 8, barHeight - 8 };
    canvas.fillRoundRect(icon.x, icon.y, icon.width, icon.height, 6, theme.accent);
    drawArrow(canvas, theme, icon);
    const int textX = barHeight + 2;
    int textWidth = width - textX - PADDING;
    stopArea = {};
    if (stopShown) {
        // An X on a framed square at the right end
        const int size = barHeight - 12;
        stopArea = { width - size - 6, (barHeight - size) / 2, size, size };
        canvas.fillRoundRect(stopArea.x, stopArea.y, size, size, 6, theme.border);
        canvas.fillRoundRect(stopArea.x + 1, stopArea.y + 1, size - 2, size - 2, 5, theme.panel);
        const float cx = static_cast<float>(stopArea.x) + size / 2.0f;
        const float cy = static_cast<float>(stopArea.y) + size / 2.0f;
        const float arm = static_cast<float>(size) * 0.22f;
        canvas.line(cx - arm, cy - arm, cx + arm, cy + arm, 2.5f, theme.text);
        canvas.line(cx - arm, cy + arm, cx + arm, cy - arm, 2.5f, theme.text);
        textWidth = stopArea.x - textX - 4;
    }
    const int textY = (barHeight - 2 * line) / 2;
    drawClippedText(canvas, font, textX, textY, instructionText, textWidth, theme.text);
    drawClippedText(canvas, font, textX, textY + line, remainingText, textWidth, theme.muted);
}

} // namespace maps
