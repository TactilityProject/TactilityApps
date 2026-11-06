// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Widgets.h"

#include <cstdint>

namespace maps {

/**
 * A small sheet at the bottom of the map for one point: holding the map (or the position button, or P) opens it for
 * a new waypoint, tapping a pin opens it for that waypoint.
 *
 * New: a name field with the next free name selected, so typing replaces it; Enter or Save keeps it, Esc, < or a tap
 * outside cancels. Boards without a keyboard keep the name. Existing: the name, with Rename, Delete and Close.
 * Under the name: the coordinates, then distance and bearing from the user's position.
 */
class WaypointSheet {
public:
    static constexpr int NAME_LENGTH = 48;

    enum class Mode : uint8_t {
        New,
        Existing
    };

    enum class Button : uint8_t {
        None,
        Save,
        Cancel,
        Rename,
        Delete,
        Close
    };

    void openNew(const char* defaultName, const char* coordinates, const char* distance, bool editable);
    void openExisting(const char* name, const char* coordinates, const char* distance);
    /** New details, for example when the position changed. */
    void setDistance(const char* distance);
    void close() { visible = false; }
    bool isOpen() const { return visible; }
    Mode mode() const { return kind; }
    const char* name() const { return value; }

    void layout(int width, int height, Font& font);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);
    bool contains(int x, int y) const { return frame.contains(x, y); }
    const Rect& bounds() const { return frame; }

    /** Typing edits the name of a new waypoint; Enter presses the focused button, Tab and arrows move the focus. */
    Button handleKey(uint32_t key);
    /** A button, or Cancel and Close for a tap outside the sheet or on its <. */
    Button handleTap(int x, int y);

private:
    int buttonCount() const { return kind == Mode::New ? 2 : 3; }
    Button buttonAt(int index) const;
    void append(uint32_t codepoint);
    void erase();

    bool visible = false;
    Mode kind = Mode::New;
    bool editable = false;
    /** The whole name is selected: the first typed character replaces it. */
    bool selectedAll = false;
    int focus = 0;
    char value[NAME_LENGTH] = "";
    char coordinateText[48] = "";
    char distanceText[64] = "";
    Rect frame {};
    Rect back {};
    Rect field {};
    Rect buttons[3] {};
};

} // namespace maps
