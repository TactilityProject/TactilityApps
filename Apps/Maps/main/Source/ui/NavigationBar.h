// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../route/RouteTurn.h"
#include "Widgets.h"

#include <cstdint>

namespace maps {

/**
 * The bar at the top of the map while following a route: an arrow for the next turn on the left, the instruction
 * ("In 120 m turn left onto Main Street") on the first line and what remains of the route ("2.1 km · 28 min left")
 * on the second. setMessage() shows a short message ("Off route - recalculating") instead of the instruction.
 * With showStop(true) a button with an X at the right end stops the navigation (stopHit()).
 */
class NavigationBar {
public:
    static constexpr int INSTRUCTION_LENGTH = 96;
    static constexpr int REMAINING_LENGTH = 48;

    /** Replaces the instruction, and clears a message. */
    void set(Turn turn, const char* instruction, const char* remaining);
    /** A one-line message instead of the instruction, until the next set(). */
    void setMessage(const char* message);
    bool hasMessage() const { return messageShown; }
    void showStop(bool value) { stopShown = value; }
    /** Whether x, y is on the stop button (a little larger than drawn, for fingers). */
    bool stopHit(int x, int y) const;
    Turn turn() const { return nextTurn; }

    /** Two text lines and padding; the arrow area is this high and wide. */
    int height(Font& font) const;
    /** A full-width panel at the top of the canvas. */
    void draw(Canvas& canvas, Font& font, const UiTheme& theme, int width);

private:
    void drawArrow(Canvas& canvas, const UiTheme& theme, const Rect& area) const;

    Turn nextTurn = Turn::Straight;
    bool messageShown = false;
    bool stopShown = false;
    Rect stopArea {};
    char instructionText[INSTRUCTION_LENGTH] = "";
    char remainingText[REMAINING_LENGTH] = "";
};

} // namespace maps
