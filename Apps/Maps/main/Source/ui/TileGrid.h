// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Canvas.h"
#include "MenuIcons.h"
#include "Widgets.h"

#include <cstdint>

namespace maps {

/** What an input did to a tile grid. */
enum class GridResult : uint8_t {
    None,
    Chosen,
    Back
};

struct GridEvent {
    GridResult result;
    /** The action of the chosen tile, status item or corner button; only set when result is Chosen. */
    uint16_t action;
};

/**
 * A full screen menu of tiles (an icon with a label under it) in a grid, a 2-D neighbour of ListView. Under the header
 * with its back chevron and an optional corner button it can show a strip of up to three status items (a running
 * download, the route), then the tiles, page dots when the tiles need more than one page, and a hint line.
 *
 * Tiles are at most about 8 x 7 line heights and the grid is centred, so a big screen shows a compact menu rather than
 * slabs; on a screen too low for readable tiles a page shows fewer rows and the rest moves to more pages.
 *
 * One focus moves over three zones: the corner button, the status strip and the tiles. As in ListView the highlight
 * stays hidden until the first key or trackball step, and touch never shows it.
 */
class TileGrid {
public:
    static constexpr int MAX_TILES = 24;
    static constexpr int MAX_STATUS = 3;
    static constexpr int LABEL_LENGTH = 16;
    static constexpr int STATUS_LENGTH = 28;
    static constexpr int TITLE_LENGTH = 32;
    static constexpr int HINT_LENGTH = 64;
    /** What focusAction() returns when nothing can be focused. */
    static constexpr uint16_t NO_ACTION = 0xFFFF;

    /**
     * Removes the tiles, status items and the corner button and puts the focus on the first tile. The title, hint, grid
     * size and the shown highlight stay, so a menu filled again keeps them.
     */
    void clear();
    void setTitle(const char* text);
    void setGrid(int columns, int rows);
    /** Returns the tile's index, -1 when full. */
    int addTile(uint16_t action, MenuIcon icon, const char* label, const char* detail = nullptr);
    /** Changes the small text of the tile with this action. An empty or null text removes it. */
    void setDetail(uint16_t action, const char* detail);
    /** Adds a status item to the strip (up to three). Without any, the strip is not shown. */
    void addStatus(uint16_t action, MenuIcon icon, const char* text);
    /** A button at the right end of the header; a null label removes it. */
    void setCorner(uint16_t action, const char* label);
    /** A line at the very bottom; a null or empty text removes it. */
    void setHint(const char* hint);

    /** Computes the rectangles. The small font is for details, the hint and the corner button (default: font). */
    void layout(int width, int height, Font& font, Font* smallFont = nullptr);
    /** Uses the fonts given to layout(), which must still exist. */
    void draw(Canvas& canvas, Font& font, const UiTheme& theme) const;

    /** Arrows and WASD move, Tab goes through everything in reading order, Enter and Space choose, Esc, Backspace and M go back. */
    GridEvent handleKey(uint32_t key);
    /** A tap on a tile, status item or the corner button chooses it, the back chevron goes back. */
    GridEvent handleTap(int x, int y);
    /** Pointer mode: the item under the pointer gets the focus and shows as hovered. */
    void hover(int x, int y);
    /** Moves the focus one step (trackball). The first step only shows the highlight. Returns whether anything changed. */
    bool move(int dx, int dy);
    /** Shows the previous (-1) or next (+1) page, for touch drags. Returns whether the page changed. */
    bool swipe(int direction);

    /** The action under the focus, NO_ACTION when there is nothing. */
    uint16_t focusAction() const;
    /** Puts the focus on the item with this action, to remember the last one; the page follows. */
    void setFocusAction(uint16_t action, bool shown);
    bool focusShown() const { return shown; }

    Rect tileRect(int index) const;
    Rect statusRect(int index) const;
    Rect cornerRect() const { return cornerBox; }
    /** For tests: the header's back area, the rows a page shows after layout(), the columns, and the grid's box. */
    const Rect& backRect() const { return header.back; }
    int shownRows() const { return usedRows; }
    int shownColumns() const { return columns; }
    const Rect& gridBounds() const { return gridBox; }
    const Rect& stripBounds() const { return stripBox; }
    int tileCount() const { return tiles; }
    int statusCount() const { return statuses; }
    int page() const { return currentPage; }
    int pageCount() const;

private:
    enum class Zone : uint8_t { Corner, Status, Tile };

    struct Tile {
        uint16_t action;
        MenuIcon icon;
        char label[LABEL_LENGTH];
        char detail[LABEL_LENGTH];
    };

    struct Status {
        uint16_t action;
        MenuIcon icon;
        char text[STATUS_LENGTH];
    };

    int perPage() const { return columns * usedRows; }
    bool hasCorner() const { return corner.label[0] != '\0'; }
    int ordinalCount() const;
    int ordinalOf(Zone zone, int index) const;
    void focusOrdinal(int ordinal);
    void keepFocusValid();
    void focusNearestTile(int centreX);
    void focusNearestStatus(int centreX);
    bool step(int dx, int dy);
    bool stepTile(int dx, int dy);
    bool moveFocusTo(Zone zone, int index);
    bool pick(int x, int y, Zone& zone, int& index) const;
    GridEvent chosen() const;

    struct Corner {
        uint16_t action = 0;
        char label[LABEL_LENGTH] = "";
    };

    Tile tileList[MAX_TILES] {};
    Status statusList[MAX_STATUS] {};
    Corner corner {};
    int tiles = 0;
    int statuses = 0;
    int columns = 3;
    int rows = 3;
    /** The rows a page really shows: rows, fewer when the screen is too low for readable tiles (more pages then). */
    int usedRows = 3;
    char title[TITLE_LENGTH] = "";
    char hint[HINT_LENGTH] = "";

    Zone zone = Zone::Tile;
    int focusIndex = 0;
    int currentPage = 0;
    bool shown = false;

    // Set by layout()
    int screenWidth = 0;
    int screenHeight = 0;
    HeaderLayout header {};
    Rect cornerBox {};
    Rect stripBox {};
    Rect gridBox {};
    int tileWidth = 0;
    int tileHeight = 0;
    int gridLeft = 0;
    int dotsY = 0;
    int hintY = 0;
    Font* mainFont = nullptr;
    Font* smallFontPtr = nullptr;
};

} // namespace maps
