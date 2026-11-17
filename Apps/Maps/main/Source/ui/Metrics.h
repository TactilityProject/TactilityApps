// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Canvas.h"

#include <algorithm>

namespace maps {

/**
 * The sizes and small drawing helpers every widget shares, so a sheet, a card and a list look alike. Widgets used to
 * carry their own copies of these.
 */
namespace metrics {

/** Space between a panel's edge and its content, and between neighbouring buttons. */
constexpr int PADDING = 8;
/** Extra height of a list row on top of the font's line height. */
constexpr int ROW_PADDING = 10;
/** Corner radius of panels (sheets, cards, dialogs, popups). */
constexpr int PANEL_RADIUS = 8;
/** Corner radius of buttons; their border takes 1 px. */
constexpr int BUTTON_RADIUS = 6;
/** Corner radius of text fields, outer edge; the inside is one less. */
constexpr int FIELD_RADIUS = 4;
/** How much the map is dimmed behind a popup, dialog, card or search sheet. */
constexpr uint8_t SCRIM_ALPHA = 100;

/** Minimum sizes on boards without a typing keyboard, where a finger is the main input. */
constexpr int TOUCH_ROW_HEIGHT = 36;
constexpr int TOUCH_BUTTON_HEIGHT = 36;
constexpr int TOUCH_BACK_WIDTH = 40;

/**
 * Whether the board has a typing keyboard. Without one a finger is the main input: rows, buttons and the back area get
 * the TOUCH_* sizes and no key hints show. With one the compact sizes stay (more rows fit) and sheets show their
 * shortcuts. Set from the app when the inputs are known; the default is the T-Deck.
 */
inline bool& keyboardFlag() {
    static bool value = true;
    return value;
}
inline void setKeyboard(bool value) { keyboardFlag() = value; }
inline bool hasKeyboard() { return keyboardFlag(); }
inline bool touchTargets() { return !keyboardFlag(); }

/** A list row's height: the font's line height plus ROW_PADDING, at least TOUCH_ROW_HEIGHT on touch boards. */
inline int rowHeight(int lineHeight) {
    return touchTargets() ? std::max(lineHeight + ROW_PADDING, TOUCH_ROW_HEIGHT) : lineHeight + ROW_PADDING;
}

/** A button's height: the font's line height plus extra, at least TOUCH_BUTTON_HEIGHT on touch boards. */
inline int buttonHeight(int lineHeight, int extra = 10) {
    return touchTargets() ? std::max(lineHeight + extra, TOUCH_BUTTON_HEIGHT) : lineHeight + extra;
}

/**
 * The width cap of a card, dialog or sheet: base pixels with the fonts of a 320 px screen, wider with bigger fonts
 * (14 line heights), so a card keeps its proportions to the text on a big screen. 300 and 280 stay as they are up to a
 * 21 px line.
 */
inline int widthCap(int base, int lineHeight) {
    return std::max(base, lineHeight * 14);
}

/**
 * The width of a card, dialog or sheet on a screen: as wide as the screen allows (minus margin) in portrait, where there
 * is no room to spare sideways, otherwise capped by widthCap.
 */
inline int cardWidth(int screenWidth, int screenHeight, int margin, int base, int lineHeight) {
    const int room = screenWidth - margin;
    return screenHeight > screenWidth ? room : std::min(room, widthCap(base, lineHeight));
}

/** The height of a header bar: the font's line height plus 10, at least TOUCH_HEADER_HEIGHT on touch boards (screens 240 px high or more). */
constexpr int TOUCH_HEADER_HEIGHT = 40;
inline int headerHeight(int lineHeight, int extra = 10, int screenHeight = 1000) {
    // Not on a low screen, where the finger-sized header would take a third of it
    return touchTargets() && screenHeight >= 240 ? std::max(lineHeight + extra, TOUCH_HEADER_HEIGHT) : lineHeight + extra;
}

/** The width of a header's back area, from its natural width. */
inline int backWidth(int natural) {
    return touchTargets() ? std::max(natural, TOUCH_BACK_WIDTH) : natural;
}

/** The < of a back button, drawn from lines so it needs no glyph in the font. cx, cy: the centre. */
inline void drawBackChevron(Canvas& canvas, float cx, float cy, uint16_t color, float reach = 6.0f, float width = 2.0f) {
    canvas.line(cx + reach / 2.0f, cy - reach, cx - reach / 2.0f, cy, width, color);
    canvas.line(cx - reach / 2.0f, cy, cx + reach / 2.0f, cy + reach, width, color);
}

/**
 * A thin bar at the right edge showing where the visible rows are in a longer list. trackTop/trackHeight: the area the
 * rows cover; right: the x of the edge the bar sits against.
 */
inline void drawScrollbar(Canvas& canvas, int right, int trackTop, int trackHeight, int visibleRows, int totalRows, int firstRow, uint16_t color) {
    if (totalRows <= visibleRows || visibleRows <= 0) {
        return;
    }
    const int barHeight = std::max(8, trackHeight * visibleRows / totalRows);
    const int barTop = trackTop + (trackHeight - barHeight) * firstRow / std::max(1, totalRows - visibleRows);
    canvas.fillRect(right - 3, barTop, 2, barHeight, color);
}

} // namespace metrics

} // namespace maps
