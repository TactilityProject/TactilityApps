// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileGrid.h"

#include "Metrics.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace maps {

namespace {

constexpr int MARGIN = 6;
constexpr int GAP = 4;
constexpr int TILE_RADIUS = 8;
constexpr int DOT_SPACING = 12;

/** Text centred in a width, cut with ".." when it does not fit. */
void drawCentredText(Canvas& canvas, Font& font, int x, int y, int width, const char* text, uint16_t color) {
    const int textWidth = font.textWidth(text);
    if (textWidth > width) {
        drawClippedText(canvas, font, x, y, text, width, color);
    } else {
        font.drawText(canvas, x + (width - textWidth) / 2, y, text, color);
    }
}

/** The outline of a tile or status item: a border and a panel inside, or a solid accent when focused. */
void drawFrame(Canvas& canvas, const Rect& r, bool focused, int radius, const UiTheme& theme) {
    if (focused) {
        canvas.fillRoundRect(r.x, r.y, r.width, r.height, radius, theme.accent);
        return;
    }
    canvas.fillRoundRect(r.x, r.y, r.width, r.height, radius, theme.border);
    canvas.fillRoundRect(r.x + 1, r.y + 1, r.width - 2, r.height - 2, radius - 1, theme.panel);
}

} // namespace

void TileGrid::clear() {
    tiles = 0;
    statuses = 0;
    corner.label[0] = '\0';
    zone = Zone::Tile;
    focusIndex = 0;
    currentPage = 0;
}

void TileGrid::setTitle(const char* text) {
    snprintf(title, sizeof(title), "%s", text != nullptr ? text : "");
}

void TileGrid::setGrid(int columnCount, int rowCount) {
    columns = std::clamp(columnCount, 1, 6);
    rows = std::clamp(rowCount, 1, 6);
    usedRows = rows;
    currentPage = 0;
    keepFocusValid();
}

int TileGrid::addTile(uint16_t action, MenuIcon icon, const char* label, const char* detail) {
    if (tiles >= MAX_TILES) {
        return -1;
    }
    Tile& tile = tileList[tiles];
    tile.action = action;
    tile.icon = icon;
    snprintf(tile.label, LABEL_LENGTH, "%s", label != nullptr ? label : "");
    snprintf(tile.detail, LABEL_LENGTH, "%s", detail != nullptr ? detail : "");
    return tiles++;
}

void TileGrid::setDetail(uint16_t action, const char* detail) {
    for (int i = 0; i < tiles; i++) {
        if (tileList[i].action == action) {
            snprintf(tileList[i].detail, LABEL_LENGTH, "%s", detail != nullptr ? detail : "");
            return;
        }
    }
}

void TileGrid::addStatus(uint16_t action, MenuIcon icon, const char* text) {
    if (statuses >= MAX_STATUS) {
        return;
    }
    Status& status = statusList[statuses++];
    status.action = action;
    status.icon = icon;
    snprintf(status.text, STATUS_LENGTH, "%s", text != nullptr ? text : "");
}

void TileGrid::setCorner(uint16_t action, const char* label) {
    corner.action = action;
    snprintf(corner.label, LABEL_LENGTH, "%s", label != nullptr ? label : "");
}

void TileGrid::setHint(const char* text) {
    snprintf(hint, sizeof(hint), "%s", text != nullptr ? text : "");
}

int TileGrid::pageCount() const {
    return tiles > 0 ? (tiles + perPage() - 1) / perPage() : 1;
}

// ---------------------------------------------------------------------------------------------------------------

void TileGrid::layout(int width, int height, Font& font, Font* smallFont) {
    Font& small = smallFont != nullptr ? *smallFont : font;
    mainFont = &font;
    smallFontPtr = &small;
    screenWidth = width;
    screenHeight = height;
    const int line = font.lineHeight();

    // The header is the one of ListView's full screen lists
    header = headerLayout(0, 0, width, metrics::headerHeight(line, 10, height));
    if (hasCorner()) {
        const int buttonHeight = metrics::touchTargets() && height >= 240 ? std::max(header.bar.height - 8, metrics::TOUCH_BUTTON_HEIGHT) : header.bar.height - 8;
        const int buttonWidth = small.textWidth(corner.label) + 16;
        cornerBox = { width - MARGIN - buttonWidth, (header.bar.height - 1 - buttonHeight) / 2, buttonWidth, buttonHeight };
    } else {
        cornerBox = { 0, 0, 0, 0 };
    }

    int top = header.bar.height + MARGIN;
    int stripHeight = 0;
    if (statuses > 0) {
        stripHeight = metrics::touchTargets() && height >= 240 ? std::max(line + 12, metrics::TOUCH_BUTTON_HEIGHT) : line + 12;
        top += stripHeight + GAP;
    }

    // From the bottom up: hint line, page dots, then the grid takes what is left
    int bottom = height - 3;
    hintY = 0;
    if (hint[0] != '\0') {
        hintY = height - small.lineHeight() - 3;
        bottom = hintY - 1;
    }

    // Tiles never get lower than a label with an icon needs; a lower screen shows fewer rows per page
    const int minTile = line + 14;
    auto fitRows = [&](int room) {
        int fit = rows;
        while (fit > 1 && (room - GAP * (fit - 1)) / fit < minTile) {
            fit--;
        }
        return fit;
    };
    dotsY = 0;
    usedRows = fitRows(bottom - 3 - top);
    if (pageCount() > 1) {
        dotsY = bottom - 5;
        bottom -= 10;
        usedRows = fitRows(bottom - 3 - top);
    }
    const int gridBottom = bottom - 3;
    const int availableHeight = gridBottom - top;

    // A tile is at most 8 x 7 line heights: more would be a slab with a small icon in it
    const int availableWidth = width - 2 * MARGIN;
    const int fullWidth = std::max(8, (availableWidth - GAP * (columns - 1)) / columns);
    const int fullHeight = std::max(8, (availableHeight - GAP * (usedRows - 1)) / usedRows);
    tileWidth = std::min(fullWidth, 8 * line);
    tileHeight = std::min(fullHeight, 7 * line);
    const bool capped = tileWidth < fullWidth || tileHeight < fullHeight;
    gridLeft = MARGIN + (availableWidth - columns * tileWidth - GAP * (columns - 1)) / 2;
    int gridTop = top;
    if (tileHeight < fullHeight) {
        gridTop += (availableHeight - (usedRows * tileHeight + GAP * (usedRows - 1))) / 2;
    }
    gridBox = { gridLeft, gridTop, columns * tileWidth + GAP * (columns - 1), usedRows * tileHeight + GAP * (usedRows - 1) };
    if (statuses > 0) {
        // The strip is as wide as the grid under it
        const int stripLeft = capped ? gridBox.x : MARGIN;
        stripBox = { stripLeft, header.bar.height + MARGIN, capped ? gridBox.width : width - 2 * MARGIN, stripHeight };
    } else {
        stripBox = { 0, 0, 0, 0 };
    }
    keepFocusValid();
}

Rect TileGrid::tileRect(int index) const {
    if (index < 0 || index >= tiles) {
        return { 0, 0, 0, 0 };
    }
    const int local = index % perPage();
    return { gridLeft + (local % columns) * (tileWidth + GAP), gridBox.y + (local / columns) * (tileHeight + GAP), tileWidth, tileHeight };
}

Rect TileGrid::statusRect(int index) const {
    if (index < 0 || index >= statuses) {
        return { 0, 0, 0, 0 };
    }
    const int width = (stripBox.width - GAP * (statuses - 1)) / statuses;
    return { stripBox.x + index * (width + GAP), stripBox.y, width, stripBox.height };
}

void TileGrid::draw(Canvas& canvas, Font& font, const UiTheme& theme) const {
    Font& small = smallFontPtr != nullptr ? *smallFontPtr : font;

    // Opaque: the map's scale bar and labels showed through between the tiles and under the hint
    canvas.fillRect(0, 0, canvas.width, canvas.height, theme.panel, 255);

    // The title stops before the corner button; the line under the header runs the full width
    HeaderLayout bar = header;
    if (hasCorner()) {
        bar.bar.width = cornerBox.x - 2;
        canvas.fillRect(bar.bar.width, header.bar.height - 1, canvas.width - bar.bar.width, 1, theme.border);
    }
    drawHeader(canvas, font, theme, bar, title, false);
    if (hasCorner()) {
        drawButton(canvas, small, theme, cornerBox, corner.label, shown && zone == Zone::Corner);
    }

    for (int i = 0; i < statuses; i++) {
        const Rect r = statusRect(i);
        const bool focused = shown && zone == Zone::Status && focusIndex == i;
        const uint16_t color = focused ? theme.accentText : theme.text;
        drawFrame(canvas, r, focused, 7, theme);
        const int statusIcon = std::min(18 * std::max(16, font.lineHeight()) / 16, r.height - 6);
        drawMenuIcon(canvas, statusList[i].icon, r.x + 5 + statusIcon / 2, r.y + r.height / 2, statusIcon, color);
        const int textX = r.x + 5 + statusIcon + 4;
        drawClippedText(canvas, small, textX, r.y + (r.height - small.lineHeight()) / 2, statusList[i].text, r.x + r.width - 4 - textX, color);
    }

    // A detail slot is kept free on every tile when any tile has a detail, so icons in a row line up
    bool anyDetail = false;
    for (int i = 0; i < tiles; i++) {
        anyDetail = anyDetail || tileList[i].detail[0] != '\0';
    }
    // The icon grows with the font, and shrinks when the tile is too low for it and a label
    int iconSize = (tileHeight < 52 ? 18 : 24) * std::max(16, font.lineHeight()) / 16;
    iconSize = std::max(10, std::min(iconSize, tileHeight - 4 - 2 - font.lineHeight()));
    const int withDetail = iconSize + 2 + font.lineHeight() + 1 + small.lineHeight();
    const bool reserve = anyDetail && withDetail <= tileHeight - 4;
    const int content = reserve ? withDetail : iconSize + 2 + font.lineHeight();

    const int first = currentPage * perPage();
    const int last = std::min(tiles, first + perPage());
    for (int i = first; i < last; i++) {
        const Rect r = tileRect(i);
        const Tile& tile = tileList[i];
        const bool focused = shown && zone == Zone::Tile && focusIndex == i;
        const uint16_t color = focused ? theme.accentText : theme.text;
        drawFrame(canvas, r, focused, TILE_RADIUS, theme);
        const int top = r.y + (r.height - content) / 2;
        const int labelY = top + iconSize + 2;
        const uint16_t detailColor = focused ? theme.accentText : theme.muted;
        if (!reserve && tile.detail[0] != '\0') {
            // Too low for a third line: the detail goes beside the icon, the pair centred
            const int room = r.width - 6 - iconSize - 4;
            const int detailWidth = std::min(small.textWidth(tile.detail), room);
            const int left = r.x + (r.width - (iconSize + 4 + detailWidth)) / 2;
            drawMenuIcon(canvas, tile.icon, left + iconSize / 2, top + iconSize / 2, iconSize, color);
            drawClippedText(canvas, small, left + iconSize + 4, top + (iconSize - small.lineHeight()) / 2, tile.detail, detailWidth, detailColor);
        } else {
            drawMenuIcon(canvas, tile.icon, r.x + r.width / 2, top + iconSize / 2, iconSize, color);
        }
        // A label too wide for the tile takes the small font before it is cut
        Font& labelFont = font.textWidth(tile.label) > r.width - 6 && small.textWidth(tile.label) <= r.width - 6 ? small : font;
        drawCentredText(canvas, labelFont, r.x + 3, labelY + (font.lineHeight() - labelFont.lineHeight()) / 2, r.width - 6, tile.label, color);
        if (reserve && tile.detail[0] != '\0') {
            drawCentredText(canvas, small, r.x + 3, labelY + font.lineHeight() + 1, r.width - 6, tile.detail, detailColor);
        }
    }

    if (dotsY > 0) {
        const int count = pageCount();
        const int left = canvas.width / 2 - (count - 1) * DOT_SPACING / 2;
        for (int i = 0; i < count; i++) {
            const auto x = static_cast<float>(left + i * DOT_SPACING);
            if (i == currentPage) {
                canvas.fillCircle(x, static_cast<float>(dotsY), 3.0f, theme.text);
            } else {
                canvas.ring(x, static_cast<float>(dotsY), 2.5f, 1.2f, theme.muted);
            }
        }
    }
    if (hintY > 0) {
        drawCentredText(canvas, small, MARGIN, hintY, canvas.width - 2 * MARGIN, hint, theme.muted);
    }
}

// ---------------------------------------------------------------------------------------------------------------

void TileGrid::keepFocusValid() {
    currentPage = std::clamp(currentPage, 0, pageCount() - 1);
    const bool valid = (zone == Zone::Corner && hasCorner()) || (zone == Zone::Status && focusIndex < statuses) ||
        (zone == Zone::Tile && focusIndex < tiles);
    if (!valid) {
        if (tiles > 0) {
            zone = Zone::Tile;
            focusIndex = std::min(focusIndex, tiles - 1);
        } else if (statuses > 0) {
            zone = Zone::Status;
            focusIndex = 0;
        } else {
            zone = hasCorner() ? Zone::Corner : Zone::Tile;
            focusIndex = 0;
        }
    }
    if (zone == Zone::Tile && tiles > 0) {
        // Keep the page the focus is on, a grid refilled with fewer tiles may have lost its page
        focusIndex = std::clamp(focusIndex, 0, tiles - 1);
        currentPage = focusIndex / perPage();
    }
}

uint16_t TileGrid::focusAction() const {
    switch (zone) {
        case Zone::Corner:
            return hasCorner() ? corner.action : NO_ACTION;
        case Zone::Status:
            return focusIndex < statuses ? statusList[focusIndex].action : NO_ACTION;
        case Zone::Tile:
            return focusIndex < tiles ? tileList[focusIndex].action : NO_ACTION;
    }
    return NO_ACTION;
}

void TileGrid::setFocusAction(uint16_t action, bool show) {
    shown = show;
    for (int i = 0; i < tiles; i++) {
        if (tileList[i].action == action) {
            moveFocusTo(Zone::Tile, i);
            return;
        }
    }
    for (int i = 0; i < statuses; i++) {
        if (statusList[i].action == action) {
            moveFocusTo(Zone::Status, i);
            return;
        }
    }
    if (hasCorner() && corner.action == action) {
        moveFocusTo(Zone::Corner, 0);
    }
}

bool TileGrid::moveFocusTo(Zone newZone, int index) {
    const bool changed = newZone != zone || index != focusIndex;
    zone = newZone;
    focusIndex = index;
    if (zone == Zone::Tile) {
        currentPage = index / perPage();
    }
    return changed;
}

void TileGrid::focusNearestTile(int centreX) {
    const int first = currentPage * perPage();
    int best = first;
    int bestDistance = 1 << 30;
    for (int c = 0; c < columns && first + c < tiles; c++) {
        const Rect r = tileRect(first + c);
        const int distance = std::abs(r.x + r.width / 2 - centreX);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = first + c;
        }
    }
    moveFocusTo(Zone::Tile, best);
}

void TileGrid::focusNearestStatus(int centreX) {
    int best = 0;
    int bestDistance = 1 << 30;
    for (int i = 0; i < statuses; i++) {
        const Rect r = statusRect(i);
        const int distance = std::abs(r.x + r.width / 2 - centreX);
        if (distance < bestDistance) {
            bestDistance = distance;
            best = i;
        }
    }
    moveFocusTo(Zone::Status, best);
}

bool TileGrid::stepTile(int dx, int dy) {
    const int pp = perPage();
    const int i = focusIndex;
    const int page = i / pp;
    const int row = (i % pp) / columns;
    const int column = i % columns;
    if (dx > 0) {
        if (column < columns - 1 && i + 1 < tiles) {
            return moveFocusTo(Zone::Tile, i + 1);
        }
        if ((page + 1) * pp < tiles) {
            // Next page, same row, first column (or the last tile when that row is not filled)
            return moveFocusTo(Zone::Tile, std::min((page + 1) * pp + row * columns, tiles - 1));
        }
        return false;
    }
    if (dx < 0) {
        if (column > 0) {
            return moveFocusTo(Zone::Tile, i - 1);
        }
        if (page > 0) {
            return moveFocusTo(Zone::Tile, (page - 1) * pp + row * columns + columns - 1);
        }
        return false;
    }
    if (dy > 0) {
        if (row < usedRows - 1) {
            const int below = std::min(i + columns, tiles - 1);
            return below > i && moveFocusTo(Zone::Tile, below);
        }
        return false;
    }
    if (row > 0) {
        return moveFocusTo(Zone::Tile, i - columns);
    }
    const Rect r = tileRect(i);
    if (statuses > 0) {
        focusNearestStatus(r.x + r.width / 2);
        return true;
    }
    if (hasCorner()) {
        return moveFocusTo(Zone::Corner, 0);
    }
    return false;
}

bool TileGrid::step(int dx, int dy) {
    switch (zone) {
        case Zone::Tile:
            return stepTile(dx, dy);
        case Zone::Status:
            if (dx != 0) {
                const int next = std::clamp(focusIndex + dx, 0, statuses - 1);
                return moveFocusTo(Zone::Status, next);
            }
            if (dy < 0) {
                return hasCorner() && moveFocusTo(Zone::Corner, 0);
            }
            if (tiles > 0) {
                const Rect r = statusRect(focusIndex);
                focusNearestTile(r.x + r.width / 2);
                return true;
            }
            return false;
        case Zone::Corner:
            if (dy > 0) {
                if (statuses > 0) {
                    focusNearestStatus(cornerBox.x + cornerBox.width / 2);
                    return true;
                }
                if (tiles > 0) {
                    focusNearestTile(cornerBox.x + cornerBox.width / 2);
                    return true;
                }
            }
            return false;
    }
    return false;
}

bool TileGrid::move(int dx, int dy) {
    if (dx == 0 && dy == 0) {
        return false;
    }
    keepFocusValid();
    if (!shown) {
        // The first step shows the highlight where the focus is, it does not move yet
        shown = true;
        return true;
    }
    return step(dx, dy);
}

int TileGrid::ordinalCount() const {
    return (hasCorner() ? 1 : 0) + statuses + tiles;
}

int TileGrid::ordinalOf(Zone z, int index) const {
    const int cornerCount = hasCorner() ? 1 : 0;
    switch (z) {
        case Zone::Corner:
            return 0;
        case Zone::Status:
            return cornerCount + index;
        case Zone::Tile:
            return cornerCount + statuses + index;
    }
    return 0;
}

void TileGrid::focusOrdinal(int ordinal) {
    const int cornerCount = hasCorner() ? 1 : 0;
    if (ordinal < cornerCount) {
        moveFocusTo(Zone::Corner, 0);
    } else if (ordinal < cornerCount + statuses) {
        moveFocusTo(Zone::Status, ordinal - cornerCount);
    } else {
        moveFocusTo(Zone::Tile, ordinal - cornerCount - statuses);
    }
}

GridEvent TileGrid::chosen() const {
    const uint16_t action = focusAction();
    if (action == NO_ACTION) {
        return { GridResult::None, 0 };
    }
    return { GridResult::Chosen, action };
}

GridEvent TileGrid::handleKey(uint32_t key) {
    const GridEvent none { GridResult::None, 0 };
    switch (key) {
        case 'w':
        case 'W':
        case keys::UP:
            move(0, -1);
            return none;
        case 's':
        case 'S':
        case keys::DOWN:
            move(0, 1);
            return none;
        case 'a':
        case 'A':
        case keys::LEFT:
            move(-1, 0);
            return none;
        case 'd':
        case 'D':
        case keys::RIGHT:
            move(1, 0);
            return none;
        case keys::TAB:
        case keys::FOCUS_NEXT:
        case keys::FOCUS_PREVIOUS: {
            keepFocusValid();
            const int count = ordinalCount();
            if (!shown) {
                shown = true;
            } else if (count > 0) {
                const int direction = key == keys::FOCUS_PREVIOUS ? count - 1 : 1;
                focusOrdinal((ordinalOf(zone, focusIndex) + direction) % count);
            }
            return none;
        }
        case keys::ENTER:
        case ' ':
        case keys::YES:
            if (!shown) {
                shown = true;
                return none;
            }
            return chosen();
        case keys::ESCAPE:
        case keys::BACKSPACE:
        case keys::NO:
        case 'm':
        case 'M':
            return { GridResult::Back, 0 };
        default:
            return none;
    }
}

bool TileGrid::pick(int x, int y, Zone& pickedZone, int& index) const {
    if (hasCorner() && cornerBox.contains(x, y)) {
        pickedZone = Zone::Corner;
        index = 0;
        return true;
    }
    for (int i = 0; i < statuses; i++) {
        if (statusRect(i).contains(x, y)) {
            pickedZone = Zone::Status;
            index = i;
            return true;
        }
    }
    const int first = currentPage * perPage();
    const int last = std::min(tiles, first + perPage());
    for (int i = first; i < last; i++) {
        if (tileRect(i).contains(x, y)) {
            pickedZone = Zone::Tile;
            index = i;
            return true;
        }
    }
    return false;
}

GridEvent TileGrid::handleTap(int x, int y) {
    if (header.back.contains(x, y)) {
        return { GridResult::Back, 0 };
    }
    Zone pickedZone;
    int index;
    if (!pick(x, y, pickedZone, index)) {
        return { GridResult::None, 0 };
    }
    // The focus follows the tap without showing the highlight, as ListView::select does
    moveFocusTo(pickedZone, index);
    return chosen();
}

void TileGrid::hover(int x, int y) {
    Zone pickedZone;
    int index;
    if (pick(x, y, pickedZone, index)) {
        moveFocusTo(pickedZone, index);
        shown = true;
    }
}

bool TileGrid::swipe(int direction) {
    const int target = std::clamp(currentPage + (direction < 0 ? -1 : 1), 0, pageCount() - 1);
    if (target == currentPage || direction == 0) {
        return false;
    }
    currentPage = target;
    if (zone == Zone::Tile) {
        // The focus stays in the same place of the new page
        const int local = focusIndex % perPage();
        focusIndex = std::min(target * perPage() + local, tiles - 1);
    }
    return true;
}

} // namespace maps
