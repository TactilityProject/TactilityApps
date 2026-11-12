// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Widgets.h"

#include <cstdint>

namespace maps {

/**
 * The route preview: a bar of the three modes (walk, bike, car) at the top of the screen, and a card at the bottom
 * with a summary line ("2.4 km · 31 min · Walk"), an optional note (for Car, wrapped to up to three lines) and the
 * buttons Start and Close. While a route is being searched it shows "Finding a route..."; Start does nothing then,
 * nor when the search failed (the card shows why, so another mode can be tried). A mode can be chosen at any time.
 *
 * Call layout() again after open(), setSummary() or setNote(), because the card's height depends on the note.
 */
class RouteCard {
public:
    static constexpr int SUMMARY_LENGTH = 64;
    static constexpr int NOTE_LENGTH = 160;
    static constexpr int NOTE_LINES = 3;
    static constexpr int NOTE_LINE_LENGTH = 96;
    static constexpr int MODE_COUNT = 3;

    enum class Button : uint8_t {
        None,
        Start,
        /** Cycles to the next mode (key P). */
        Profile,
        Close,
        Walk,
        Bike,
        Car
    };

    /** note: a second text, wrapped to the card's width; nullptr or "" for none. Focus starts on Start. */
    void open(const char* summary, const char* note);
    void setSummary(const char* summary);
    void setNote(const char* note);
    void close() { visible = false; }
    bool isOpen() const { return visible; }
    /** While busy the card shows "Finding a route..." instead of the summary, and Start does nothing. */
    void setBusy(bool value) { busy = value; }
    bool isBusy() const { return busy; }
    /** Without a route (the search failed) Start does nothing. */
    void setCanStart(bool value) { canStart = value; }
    /** A route is shown that Start would follow. */
    bool canStartRoute() const { return canStart && !busy; }
    /** The mode shown as chosen in the bar: 0 walk, 1 bike, 2 car (RouteProfile). */
    void setMode(int value) { mode = value; }
    int selectedMode() const { return mode; }
    /** The button Enter would press. */
    Button focused() const { return buttonAt(focus); }

    void layout(int width, int height, Font& font);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);
    /** The card or the mode bar. */
    bool contains(int x, int y) const { return frame.contains(x, y) || modeBar.contains(x, y); }
    const Rect& bounds() const { return frame; }
    const Rect& modeBounds() const { return modeBar; }

    /**
     * Tab and arrows move the focus over the modes and the buttons, Enter presses the focused one, W, B, C (or 1, 2, 3)
     * choose a mode, P the next one, S is Start, Esc and Backspace close.
     */
    Button handleKey(uint32_t key);
    /** A button or mode, or Close for a tap outside the card and the bar. */
    Button handleTap(int x, int y);

private:
    /** The modes, then Start and Close. */
    static constexpr int FOCUS_COUNT = MODE_COUNT + 2;
    static constexpr int FOCUS_START = MODE_COUNT;

    static Button buttonAt(int index);
    /** Start does nothing while busy or without a route. */
    Button pressed(Button button) const;

    bool visible = false;
    bool busy = false;
    bool canStart = true;
    int mode = 0;
    int focus = FOCUS_START;
    int noteLineCount = 0;
    char summaryText[SUMMARY_LENGTH] = "";
    char noteText[NOTE_LENGTH] = "";
    char noteLines[NOTE_LINES][NOTE_LINE_LENGTH] {};
    Rect frame {};
    Rect modeBar {};
    Rect modes[MODE_COUNT] {};
    Rect buttons[2] {};
};

/** The icon of a mode (0 walk, 1 bike, 2 car) drawn in lines, centred on cx, cy, about 22 x 18 px. */
void drawModeIcon(Canvas& canvas, int modeIndex, float cx, float cy, uint16_t color);

} // namespace maps
