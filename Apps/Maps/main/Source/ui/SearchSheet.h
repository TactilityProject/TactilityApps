// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Widgets.h"

#include <cstdint>

namespace maps {

/**
 * The search sheet: a full screen overlay over the dimmed map with a < at the top left, a text field with a cursor
 * (placeholder "Search places, streets, addresses"), a note line under it, and a list of rows. It knows nothing about
 * how the results are found: the caller passes plain row data and reacts to the events.
 *
 * Rows: the name, under it the type in plain words with the distance and a small arrow (rotated by the bearing) on the
 * right, and an optional third line with a note ("Street estimated from the map", "Also: X, Y"). With the on-screen
 * keyboard (compact layout, see layout) a row is one line: name, type, distance.
 *
 * Empty field: the list shows the entry "Nearby by category" (opens the category list inside the sheet) and the recent
 * rows. A non-empty field shows the result rows. The caller answers every TextChanged event with setRows (an empty
 * list when the text is too short to search): the sheet keeps showing the old rows until it does.
 *
 * Rows come in pages: setRows gives the first page, the sheet raises WantMore when the selection or the scroll position
 * nears the end of a list that has more, and the caller answers with appendRows. At most MAX_ROWS rows are kept.
 *
 * Keys: typing edits the field (UTF-8, code points as Tactility sends them), Backspace deletes, Backspace on an empty
 * field closes (in the category list: back), Esc closes (in the category list: back), Enter opens the selected row,
 * Up/Down (the trackball) and focus keys move the selection, Tab moves the focus between the field and the list.
 * Touch: a tap on a row opens it, dragging scrolls, a tap on the field asks for the keyboard (FocusField), < closes.
 * A trackball in mouse mode uses rowAt/hover/move, as MapView does for its lists.
 */
class SearchSheet {
public:
    static constexpr int TEXT_LENGTH = 64;
    static constexpr int MAX_ROWS = 64;
    static constexpr int MAX_RECENT = 8;
    static constexpr int MAX_CATEGORIES = 16;
    /** How many rows the caller gives at once. */
    static constexpr int PAGE_ROWS = 8;

    /** One row, plain texts (copied, the pointers need not outlive the call). Empty strings and nullptr are the same. */
    struct Row {
        const char* name;
        /** The type in plain words: Cafe, Street, Village, Address */
        const char* type;
        /** Optional third line */
        const char* note;
        /** "850 m", "2.4 km"; empty: no distance and no arrow */
        const char* distance;
        /** Direction of the place in degrees, clockwise, 0 = up on the screen (the caller makes it relative to north-up or heading-up) */
        float bearing;
    };

    enum class EventType : uint8_t {
        None,
        /** The text changed: query again (from 3 characters) and call setRows */
        TextChanged,
        /** A result row was chosen; index counts the rows given to setRows/appendRows */
        OpenRow,
        /** A recent row was chosen; index counts the rows given to setRecent */
        OpenRecent,
        /** A category was chosen; index counts the names given to setCategories */
        OpenCategory,
        Close,
        /** The list nears its end and has more: call appendRows */
        WantMore,
        /** The field was tapped: show the on-screen keyboard (touch boards) */
        FocusField
    };

    struct Event {
        EventType type = EventType::None;
        int index = -1;
    };

    void open();
    void close() { visible = false; }
    bool isOpen() const { return visible; }
    /** Boards with a typing keyboard show key hints and highlight the selection from the start. */
    void setTypingKeyboard(bool value);
    bool hasTypingKeyboard() const { return typing; }

    const char* text() const { return value; }
    /** Replaces the text without an event (for example a recent search). */
    void setText(const char* text);
    /** The line under the field; empty hides it. */
    void setHeader(const char* text);
    const char* header() const { return headerText; }

    /** Replaces the result rows (the first page); selects the first. more: the caller has further rows. */
    void setRows(const Row* rows, int count, bool more);
    void appendRows(const Row* rows, int count, bool more);
    int rowCount() const { return resultCount; }
    /** Rows shown with an empty field, after "Nearby by category". */
    void setRecent(const Row* rows, int count);
    /** The names of the category list (12 at the moment). With none the entry "Nearby by category" is not shown. */
    void setCategories(const char* const* names, int count);
    /** Shows the category list (the entry "Nearby by category" does this by itself). */
    void showCategories();
    /** Shows the result rows although no text is typed (a nearby list); Back returns to the categories. */
    void showResults();
    bool inCategories() const { return categoryView; }
    /** True when the list nears its end, more rows exist and the caller was not asked yet for this many rows. */
    bool wantsMore() const;

    /**
     * Fills width x height from the top left. height is the room above the on-screen keyboard when compact (then rows
     * are one line, no hints), the whole screen height otherwise.
     */
    void layout(int width, int height, Font& font, bool compact = false);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);
    const Rect& bounds() const { return frame; }
    const Rect& fieldBounds() const { return field; }
    bool fieldFocused() const { return focusField; }

    Event handleKey(uint32_t key);
    /** A tap on the sheet (the caller sends taps on the on-screen keyboard to TouchKeyboard::tap and the key here). */
    Event handleTap(int x, int y);
    /** A finger moved dy pixels (positive: down) while dragging on the list: scrolls whole rows, the selection stays. */
    Event handleDrag(int dy);
    /** The end of a drag: forget the part of a row. */
    void endDrag() { dragPixels = 0; }
    /** The selection moves by rows (trackball steps); the first step only shows the highlight. */
    Event move(int rows);
    /** Scrolls the list by rows, the selection stays. */
    Event scrollBy(int rows);
    /** The entry (a row of the list as shown) under a screen position, -1 for none. */
    int rowAt(int x, int y) const;
    /** The pointer is over an entry: it becomes the highlighted one. */
    void hover(int entry);
    int selected() const { return selection; }
    int firstVisible() const { return first; }
    int entryCount() const;
    /** Pixels a scrolling step of one entry takes, for mapping pointer or drag movement to rows. */
    int rowHeight() const;
    /** For tests: the rectangle of an entry when visible. */
    bool entryRect(int entry, Rect& rect) const;

private:
    struct Slot {
        char name[64];
        /** The type line, with the settlement: "Street · Holice" */
        char type[72];
        char note[64];
        char distance[12];
        float bearing;
    };
    enum class Source : uint8_t { Browse, Recent, Category, Result };

    static void store(Slot& slot, const Row& row);
    Source sourceOf(int entry, int& index) const;
    const Slot* slotOf(int entry) const;
    int entryHeight(int entry) const;
    int fitting(int firstEntry) const;
    int maxFirst() const;
    void keepVisible();
    void relayout();
    Event activate(int entry);
    Event openSelected();
    Event goBack();
    Event wantMoreIfNeeded(Event event);
    void append(uint32_t codepoint);
    bool erase();
    void drawArrow(Canvas& canvas, float cx, float cy, float degrees, uint16_t color) const;

    bool visible = false;
    bool typing = false;
    bool compact = false;
    bool focusField = true;
    bool highlighted = false;
    bool categoryView = false;
    /** Result rows without typed text (a nearby list). */
    bool resultView = false;
    bool moreRows = false;
    int askedAt = -1;
    int selection = 0;
    int first = 0;
    int dragPixels = 0;
    char value[TEXT_LENGTH] = "";
    char headerText[64] = "";
    Slot results[MAX_ROWS] {};
    int resultCount = 0;
    Slot recent[MAX_RECENT] {};
    int recentCount = 0;
    char categories[MAX_CATEGORIES][32] {};
    int categoryCount = 0;

    int lineHeight = 16;
    int barHeight = 28;
    int listTop = 0;
    int listBottom = 0;
    Rect frame {};
    Rect backButton {};
    Rect field {};
};

} // namespace maps
