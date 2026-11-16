// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Canvas.h"
#include "Widgets.h"

#include <cstdint>

namespace maps {

/** The buttons along the screen edges that show on demand over the map. */
enum class Control : uint8_t {
    Menu,
    ZoomIn,
    ZoomOut,
    Position,
    /** Opens the search, like the key "/"; sits under the menu button. Last in the enum so the focus order of the others stays. */
    Search,
    /** Switches the map style (Standard, Outdoor), like the key "L"; under the search button, only on colour displays. */
    Style,
    Count,
    None = Count
};

/** How the position button looks: filled while following (with a pointer in heading-up), outline when not, crossed out without a position. */
enum class PositionButton : uint8_t {
    Following,
    /** Following with the map turned to the direction of travel. */
    FollowingHeading,
    Free,
    Unavailable
};

/** Which inputs the board has, found when the app starts. */
struct InputKinds {
    bool keyboard;
    bool touch;
    bool pointer;
};

/**
 * The hidden control overlay, laid out for the screen: menu top left with search under it, zoom in and out in the middle of the
 * right edge, position bottom right above the scale bar. Buttons are about 32 px, at least 6 px from the edge,
 * semi-transparent, never over the screen centre. With a keyboard each button shows its key. On boards with
 * only buttons (no touch, no pointer) one control has the focus, which arrows move and Enter presses.
 */
class Controls {
public:
    /** The size on a 240 px screen and the smallest bigger screens get; bigger screens grow to at most MAX_SIZE. */
    static constexpr int SIZE = 32;
    static constexpr int MAX_SIZE = 48;
    static constexpr int EDGE = 6;
    /** The button size for a screen: SIZE up to 320 px, 1/10 of the short side up to MAX_SIZE, less on a low screen. */
    static int sizeFor(int width, int height);

    /** top: the height of a bar along the top edge (navigation), the menu button goes below it. */
    void layout(int width, int height, int scaleBarTop, int top = 0);
    void setInputs(const InputKinds& kinds) { inputs = kinds; }
    /** Whether the map style button shows (colour displays: a 1-bit or grey display has one style). */
    void setStyleButton(bool shown) { styleButton = shown; }
    const InputKinds& inputKinds() const { return inputs; }
    /** Focus mode: no touch and no pointer, the arrow keys move between the buttons. */
    bool usesFocus() const { return !inputs.touch && !inputs.pointer; }

    bool isVisible() const { return visible; }
    void show();
    void hide() { visible = false; }
    void toggle();

    /** The visible button at a screen position, Control::None when there is none. */
    Control hit(int x, int y) const;
    const Rect& rect(Control control) const { return rects[static_cast<int>(control)]; }
    /** The key that presses a button. */
    static char keyOf(Control control);
    /** The button a key presses while the controls are visible, Control::None for other keys. */
    static Control controlForKey(uint32_t key);

    Control focused() const { return focus; }
    void moveFocus(int steps);
    /** The button under the pointer, highlighted like the focus; Control::None when the pointer is elsewhere. */
    void setHover(Control control) { hover = control; }
    Control hovered() const { return hover; }

    void draw(Canvas& canvas, Font& font, const UiTheme& theme, PositionButton position);

private:
    bool styleButton = true;
    void drawIcon(Canvas& canvas, Control control, const Rect& rect, uint16_t color, PositionButton position, const UiTheme& theme);

    int size = SIZE;
    Rect rects[static_cast<int>(Control::Count)] {};
    InputKinds inputs { true, true, false };
    bool visible = false;
    Control focus = Control::Menu;
    Control hover = Control::None;
};

/**
 * The attribution in two steps: the full credit bar for 5 s at start, then a © mark; tapping the mark shows the
 * bar again for 5 s, tapping the bar opens About and licences.
 */
class Attribution {
public:
    static constexpr uint32_t SHOW_MS = 5000;

    enum class Tap : uint8_t {
        Missed,
        Expanded,
        OpenAbout
    };

    void expand(uint32_t now);
    bool isExpanded() const { return expanded; }
    /** Collapses the bar once its time is up. Returns true when it changed. */
    bool update(uint32_t now);
    /** What a tap or click at x, y does, given the bar's box when expanded and the mark's when collapsed. */
    Tap tap(int x, int y, const Rect& bar, const Rect& mark, uint32_t now);

private:
    bool expanded = false;
    uint32_t until = 0;
};

} // namespace maps
