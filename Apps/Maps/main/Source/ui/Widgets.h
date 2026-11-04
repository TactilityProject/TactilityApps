// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Canvas.h"

#include <cstdint>

namespace maps {

/** Key code points, the same values as Tactility's CodePoint (tactility/drivers/keyboard.h). */
namespace keys {
constexpr uint32_t ENTER = 10;
constexpr uint32_t ESCAPE = 0x1B;
constexpr uint32_t BACKSPACE = '\b';
constexpr uint32_t DELETE = 0x7F;
constexpr uint32_t TAB = '\t';
constexpr uint32_t LEFT = 0x2190;
constexpr uint32_t UP = 0x2191;
constexpr uint32_t RIGHT = 0x2192;
constexpr uint32_t DOWN = 0x2193;
constexpr uint32_t YES = 0x2713;
constexpr uint32_t NO = 0x2717;
constexpr uint32_t FOCUS_NEXT = 0x21E5;
constexpr uint32_t FOCUS_PREVIOUS = 0x21E4;
} // namespace keys

struct Rect {
    int x;
    int y;
    int width;
    int height;

    bool contains(int px, int py) const { return px >= x && py >= y && px < x + width && py < y + height; }
};

/** What an input did to a list or dialog. */
enum class UiAction : uint8_t {
    None,
    Moved,
    Chosen,
    Back,
    Delete
};

/** A button with a text label, highlighted when focused. */
void drawButton(Canvas& canvas, Font& font, const UiTheme& theme, const Rect& rect, const char* label, bool focused, bool danger = false);

/** The buttons of a screen header: < goes back one level, the check mark chooses the highlighted row. */
struct HeaderLayout {
    Rect bar;
    Rect back;
    Rect select;
};

/** A header bar of height pixels across width at x, y: back button, title, and a select button when hasSelect. */
HeaderLayout headerLayout(int x, int y, int width, int height);
void drawHeader(Canvas& canvas, Font& font, const UiTheme& theme, const HeaderLayout& header, const char* title, bool hasSelect);

/**
 * A list with a highlighted row, either full screen or a popup in the middle of the screen, both under a header with
 * a back button (<), the title and a select button (a check mark) that chooses the highlighted row. Full screen lists
 * have a hint line at the bottom. Rows are at least the font's line height plus padding, about six fit a 240 px
 * screen.
 */
class ListView {
public:
    /** The longest list is the region catalog: 62 regions in North America, plus "All of ...". */
    static constexpr int MAX_ITEMS = 72;
    static constexpr int TEXT_LENGTH = 64;

    void clear();
    void setTitle(const char* text);
    /** The hint at the bottom of a full screen list. A '\n' starts a second line. */
    void setHint(const char* text);
    /** Adds a row with a label and an optional detail on the right. Returns its index, -1 when full. */
    int add(const char* label, const char* detail = "");
    void setDetail(int index, const char* detail);
    int count() const { return itemCount; }
    int selected() const { return selection; }
    /** Moves the selection without showing the highlight. */
    void select(int index);
    /**
     * Whether the highlight shows. A list opens without one: keys and the trackball show it, touch never does, so a
     * finger scrolls the list and taps rows directly.
     */
    bool isHighlighted() const { return highlighted; }
    /** clear() keeps it, so a list filled again keeps its highlight; a list that opens calls setHighlighted(false). */
    void setHighlighted(bool value) { highlighted = value; }
    /** Scrolls the rows by a number of rows, leaving the selection where it is. */
    void scrollBy(int rows);

    void layoutFullScreen(int width, int height, Font& font);
    void layoutPopup(int width, int height, Font& font, int popupWidth);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);

    /**
     * W/S, arrows and focus keys move, Enter and Space choose, Esc and Backspace go back, Del and X delete. Without a
     * highlight the first moving key or Enter only shows it on the first visible row.
     */
    UiAction handleKey(uint32_t key);
    /** A tap on a row chooses it, the check mark the highlighted row; < and a tap outside a popup go back. */
    UiAction handleTap(int x, int y);
    /** Trackball steps move the selection and show the highlight. */
    UiAction move(int rows);
    /** The row at a screen position, -1 when there is none. */
    int rowAt(int x, int y) const;
    const Rect& bounds() const { return frame; }
    int rowHeight() const { return rowPixels; }

private:
    void keepVisible();
    int hintLines() const;

    struct Item {
        char label[TEXT_LENGTH];
        char detail[TEXT_LENGTH];
    };

    Item items[MAX_ITEMS] {};
    int itemCount = 0;
    int selection = 0;
    int firstVisible = 0;
    int visibleRows = 1;
    bool highlighted = false;
    char title[TEXT_LENGTH] = "";
    char hint[TEXT_LENGTH * 2] = "";
    Rect frame {};
    HeaderLayout header {};
    int rowPixels = 24;
    int listTop = 0;
    bool fullScreen = true;
};

/** A question with a confirm and a cancel button. */
class Dialog {
public:
    void open(const char* message, const char* confirmLabel, bool danger);
    void close() { visible = false; }
    bool isOpen() const { return visible; }

    void layout(int width, int height, Font& font);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);

    /** Enter or Y confirm the focused button, Esc or N cancel, left/right/up/down/Tab move the focus. */
    UiAction handleKey(uint32_t key);
    /** Chosen for the confirm button, Back for cancel or a tap outside. */
    UiAction handleTap(int x, int y);
    /** Returns Chosen when the confirm button is focused. */
    UiAction press();

private:
    bool visible = false;
    bool danger = false;
    bool confirmFocused = false;
    char text[96] = "";
    char confirm[24] = "";
    Rect frame {};
    Rect confirmButton {};
    Rect cancelButton {};
    char lines[3][80] {};
    int lineCount = 0;
};

/**
 * A one-line text field in a dialog, for names: typed on a keyboard, Backspace deletes, Enter or OK keeps the text,
 * Esc or Cancel drops it. Boards without a keyboard can only press OK or Cancel.
 */
class TextPrompt {
public:
    static constexpr int TEXT_LENGTH = 48;

    void open(const char* title, const char* text);
    void close() { visible = false; }
    bool isOpen() const { return visible; }
    const char* text() const { return value; }

    void layout(int width, int height, Font& font);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);
    /** Chosen: OK, Back: cancelled, Moved: the text changed. */
    UiAction handleKey(uint32_t key);
    UiAction handleTap(int x, int y);

private:
    void append(uint32_t codepoint);
    void erase();

    bool visible = false;
    char heading[64] = "";
    char value[TEXT_LENGTH] = "";
    Rect frame {};
    Rect field {};
    Rect okButton {};
    Rect cancelButton {};
};

} // namespace maps
