// SPDX-License-Identifier: GPL-3.0-or-later
#include "Widgets.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr int PADDING = 8;
constexpr int ROW_PADDING = 10;

} // namespace

void drawButton(Canvas& canvas, Font& font, const UiTheme& theme, const Rect& rect, const char* label, bool focused, bool danger) {
    const uint16_t fill = focused ? (danger ? theme.danger : theme.accent) : theme.panel;
    const uint16_t text = focused ? (danger ? theme.dangerText : theme.accentText) : theme.text;
    canvas.fillRoundRect(rect.x, rect.y, rect.width, rect.height, 6, focused ? fill : theme.border);
    if (!focused) {
        canvas.fillRoundRect(rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2, 5, fill);
    }
    const int width = font.textWidth(label);
    font.drawText(canvas, rect.x + (rect.width - width) / 2, rect.y + (rect.height - font.lineHeight()) / 2, label, text);
}

// ---------------------------------------------------------------------------------------------------------------

void ListView::clear() {
    itemCount = 0;
    selection = 0;
    firstVisible = 0;
}

void ListView::setTitle(const char* text) {
    snprintf(title, sizeof(title), "%s", text);
}

void ListView::setHint(const char* text) {
    snprintf(hint, sizeof(hint), "%s", text);
}

int ListView::add(const char* label, const char* detail) {
    if (itemCount >= MAX_ITEMS) {
        return -1;
    }
    snprintf(items[itemCount].label, TEXT_LENGTH, "%s", label);
    snprintf(items[itemCount].detail, TEXT_LENGTH, "%s", detail);
    return itemCount++;
}

void ListView::setDetail(int index, const char* detail) {
    if (index >= 0 && index < itemCount) {
        snprintf(items[index].detail, TEXT_LENGTH, "%s", detail);
    }
}

void ListView::select(int index) {
    selection = std::clamp(index, 0, std::max(0, itemCount - 1));
    keepVisible();
}

void ListView::keepVisible() {
    if (selection < firstVisible) {
        firstVisible = selection;
    } else if (selection >= firstVisible + visibleRows) {
        firstVisible = selection - visibleRows + 1;
    }
    firstVisible = std::clamp(firstVisible, 0, std::max(0, itemCount - visibleRows));
}

void ListView::layoutFullScreen(int width, int height, Font& font) {
    fullScreen = true;
    rowPixels = font.lineHeight() + ROW_PADDING;
    frame = { 0, 0, width, height };
    // Title on top, hint at the bottom, rows in between
    listTop = font.lineHeight() + PADDING;
    const int hintHeight = hint[0] != '\0' ? hintLines() * font.lineHeight() + 4 : 0;
    visibleRows = std::max(1, (height - listTop - hintHeight) / rowPixels);
    keepVisible();
}

void ListView::layoutPopup(int width, int height, Font& font, int popupWidth) {
    fullScreen = false;
    rowPixels = font.lineHeight() + ROW_PADDING;
    const int maxRows = std::max(1, (height - 2 * PADDING) / rowPixels);
    visibleRows = std::min(std::max(1, itemCount), maxRows);
    const int popupHeight = visibleRows * rowPixels + PADDING;
    frame = { (width - popupWidth) / 2, (height - popupHeight) / 2, popupWidth, popupHeight };
    listTop = PADDING / 2;
    keepVisible();
}

void ListView::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    if (fullScreen) {
        canvas.fillRect(frame.x, frame.y, frame.width, frame.height, theme.panel);
        font.drawText(canvas, PADDING, 3, title, theme.text);
        canvas.fillRect(0, listTop - 3, frame.width, 1, theme.border);
    } else {
        // The map stays visible but dimmed behind a popup
        canvas.fillRect(0, 0, canvas.width, canvas.height, theme.scrim, 90);
        canvas.fillRoundRect(frame.x, frame.y, frame.width, frame.height, 8, theme.panel, theme.panelOpacity);
    }
    if (itemCount == 0 && hint[0] != '\0' && fullScreen) {
        return;
    }
    for (int row = 0; row < visibleRows && firstVisible + row < itemCount; row++) {
        const int index = firstVisible + row;
        const Item& item = items[index];
        const int y = frame.y + listTop + row * rowPixels;
        const bool highlighted = index == selection;
        const int left = frame.x + 4;
        const int width = frame.width - 8;
        if (highlighted) {
            canvas.fillRoundRect(left, y, width, rowPixels - 2, 5, theme.accent);
        }
        const uint16_t color = highlighted ? theme.accentText : theme.text;
        const int textY = y + (rowPixels - 2 - font.lineHeight()) / 2;
        const int detailWidth = item.detail[0] != '\0' ? font.textWidth(item.detail) : 0;
        drawClippedText(canvas, font, left + PADDING, textY, item.label, width - 2 * PADDING - detailWidth - (detailWidth > 0 ? PADDING : 0), color);
        if (detailWidth > 0) {
            font.drawText(canvas, left + width - PADDING - detailWidth, textY, item.detail, highlighted ? theme.accentText : theme.muted);
        }
    }
    // A thin bar shows where the visible rows are in a longer list
    if (itemCount > visibleRows) {
        const int trackTop = frame.y + listTop;
        const int trackHeight = visibleRows * rowPixels;
        const int barHeight = std::max(8, trackHeight * visibleRows / itemCount);
        const int barTop = trackTop + (trackHeight - barHeight) * firstVisible / std::max(1, itemCount - visibleRows);
        canvas.fillRect(frame.x + frame.width - 3, barTop, 2, barHeight, theme.muted);
    }
    if (fullScreen && hint[0] != '\0') {
        int y = frame.height - hintLines() * font.lineHeight() - 2;
        canvas.fillRect(0, y - 2, frame.width, 1, theme.border);
        const char* line = hint;
        while (line != nullptr) {
            const char* end = strchr(line, '\n');
            char text[TEXT_LENGTH * 2];
            const size_t length = end != nullptr ? static_cast<size_t>(end - line) : strlen(line);
            snprintf(text, sizeof(text), "%.*s", static_cast<int>(length), line);
            drawClippedText(canvas, font, PADDING, y, text, frame.width - 2 * PADDING, theme.muted);
            y += font.lineHeight();
            line = end != nullptr ? end + 1 : nullptr;
        }
    }
}

int ListView::hintLines() const {
    int lines = 1;
    for (const char* c = hint; *c != '\0'; c++) {
        lines += *c == '\n' ? 1 : 0;
    }
    return lines;
}

UiAction ListView::move(int rows) {
    if (itemCount == 0 || rows == 0) {
        return UiAction::None;
    }
    const int previous = selection;
    select(selection + rows);
    return selection != previous ? UiAction::Moved : UiAction::None;
}

UiAction ListView::handleKey(uint32_t key) {
    switch (key) {
        case 'w':
        case 'W':
        case keys::UP:
        case keys::LEFT:
        case keys::FOCUS_PREVIOUS:
            return move(-1);
        case 's':
        case 'S':
        case keys::DOWN:
        case keys::RIGHT:
        case keys::FOCUS_NEXT:
        case keys::TAB:
            return move(1);
        case keys::ENTER:
        case ' ':
        case keys::YES:
            return itemCount > 0 ? UiAction::Chosen : UiAction::None;
        case keys::ESCAPE:
        case keys::BACKSPACE:
        case keys::NO:
            return UiAction::Back;
        case keys::DELETE:
        case 'x':
        case 'X':
            return itemCount > 0 ? UiAction::Delete : UiAction::None;
        default:
            return UiAction::None;
    }
}

int ListView::rowAt(int x, int y) const {
    if (!frame.contains(x, y)) {
        return -1;
    }
    const int offset = y - frame.y - listTop;
    if (offset < 0) {
        return -1;
    }
    const int row = offset / rowPixels;
    const int index = firstVisible + row;
    return row < visibleRows && index < itemCount ? index : -1;
}

UiAction ListView::handleTap(int x, int y) {
    const int index = rowAt(x, y);
    if (index >= 0) {
        select(index);
        return UiAction::Chosen;
    }
    // A popup closes when tapped outside, a full screen list only through its title bar
    if (!fullScreen && !frame.contains(x, y)) {
        return UiAction::Back;
    }
    if (fullScreen && y < listTop) {
        return UiAction::Back;
    }
    return UiAction::None;
}

// ---------------------------------------------------------------------------------------------------------------

void Dialog::open(const char* message, const char* confirmLabel, bool isDanger) {
    snprintf(text, sizeof(text), "%s", message);
    snprintf(confirm, sizeof(confirm), "%s", confirmLabel);
    danger = isDanger;
    // Cancel is focused first, so a stray Enter never deletes anything
    confirmFocused = false;
    visible = true;
}

void Dialog::layout(int width, int height, Font& font) {
    const int dialogWidth = std::min(width - 16, 280);
    lineCount = wrapText(font, text, dialogWidth - 2 * PADDING, &lines[0][0], sizeof(lines[0]), 3);
    const int buttonHeight = font.lineHeight() + 12;
    const int dialogHeight = PADDING + lineCount * font.lineHeight() + PADDING + buttonHeight + PADDING;
    frame = { (width - dialogWidth) / 2, (height - dialogHeight) / 2, dialogWidth, dialogHeight };
    const int buttonWidth = (dialogWidth - 3 * PADDING) / 2;
    const int buttonY = frame.y + dialogHeight - PADDING - buttonHeight;
    cancelButton = { frame.x + PADDING, buttonY, buttonWidth, buttonHeight };
    confirmButton = { frame.x + 2 * PADDING + buttonWidth, buttonY, buttonWidth, buttonHeight };
}

void Dialog::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    canvas.fillRect(0, 0, canvas.width, canvas.height, theme.scrim, 110);
    canvas.fillRoundRect(frame.x, frame.y, frame.width, frame.height, 8, theme.panel);
    for (int i = 0; i < lineCount; i++) {
        font.drawText(canvas, frame.x + PADDING, frame.y + PADDING + i * font.lineHeight(), lines[i], theme.text);
    }
    drawButton(canvas, font, theme, cancelButton, "Cancel", !confirmFocused);
    drawButton(canvas, font, theme, confirmButton, confirm, confirmFocused, danger);
}

UiAction Dialog::press() {
    visible = false;
    return confirmFocused ? UiAction::Chosen : UiAction::Back;
}

UiAction Dialog::handleKey(uint32_t key) {
    switch (key) {
        case keys::LEFT:
        case keys::RIGHT:
        case keys::UP:
        case keys::DOWN:
        case keys::TAB:
        case keys::FOCUS_NEXT:
        case keys::FOCUS_PREVIOUS:
        case 'a':
        case 'A':
        case 'd':
        case 'w':
        case 'W':
        case 's':
        case 'S':
            confirmFocused = !confirmFocused;
            return UiAction::Moved;
        case keys::ENTER:
        case ' ':
            return press();
        case 'y':
        case 'Y':
        case keys::YES:
            visible = false;
            return UiAction::Chosen;
        case 'n':
        case 'N':
        case keys::NO:
        case keys::ESCAPE:
        case keys::BACKSPACE:
            visible = false;
            return UiAction::Back;
        default:
            return UiAction::None;
    }
}

UiAction Dialog::handleTap(int x, int y) {
    if (confirmButton.contains(x, y)) {
        visible = false;
        return UiAction::Chosen;
    }
    if (cancelButton.contains(x, y) || !frame.contains(x, y)) {
        visible = false;
        return UiAction::Back;
    }
    return UiAction::None;
}

} // namespace maps
