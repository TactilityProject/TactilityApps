// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Widgets.h"

#include <cstdint>

namespace maps {

/**
 * An on-screen keyboard for boards with only a touch screen. It fills the bottom of the screen while a text field is
 * open; a tap on a key becomes a key code for the text field (TextPrompt::handleKey, WaypointSheet::handleKey).
 * Pages: letters, numbers and symbols, accented letters. Shift: once = the next letter in upper case, twice = caps
 * lock, three times = off.
 */
class TouchKeyboard {
public:
    /** Height it takes at the bottom of a screen of this width (4 rows of 30 px, 2 px gaps and edges): 128 px. */
    static int heightFor(int width);

    /** Opens on the letters page, lower case. */
    void open();
    void close() { visible = false; }
    bool isOpen() const { return visible; }

    /** Fills the bottom heightFor(width) pixels of a screen of width x height. */
    void layout(int width, int height, Font& font);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);
    /** The keyboard's area, false when it is closed. */
    bool contains(int x, int y) const;

    /**
     * A tap: returns the key code to feed to the text field (a printable code point, keys::BACKSPACE, keys::ENTER), or
     * 0 when the tap only changed the keyboard itself (shift, page switch), hit no key or was outside.
     */
    uint32_t tap(int x, int y);

    /** For tests: the centre of the key with this label ("a", "Shift", "Space", "OK", "123", "ABC", "Del"...). */
    bool keyCentre(const char* label, int& x, int& y) const;
    /** For tests: the rectangle of that key. */
    bool keyRect(const char* label, Rect& rect) const;

private:
    enum class Page : uint8_t { Letters, Symbols, Accents };
    enum class Shift : uint8_t { Off, Once, Lock };
    enum class Kind : uint8_t { Character, Shift, Delete, Enter, Space, ToLetters, ToSymbols, ToAccents };

    struct Key {
        Rect rect;
        Kind kind;
        uint32_t code;
        char label[8];
    };

    static constexpr int MAX_KEYS = 40;
    static constexpr int ROW_UNITS = 40;

    void build();
    void beginRow(int row, int offsetUnits);
    void add(Kind kind, uint32_t code, const char* label, int units);
    void addCharacter(uint32_t code, int units);
    bool shifted() const { return shift != Shift::Off && page != Page::Symbols; }
    void setPage(Page next);

    bool visible = false;
    Page page = Page::Letters;
    Shift shift = Shift::Off;
    int screenWidth = 0;
    Rect area {};
    Key keys_[MAX_KEYS] {};
    int keyCount = 0;
    int rowY = 0;
    int rowCursor = 0;
};

} // namespace maps
