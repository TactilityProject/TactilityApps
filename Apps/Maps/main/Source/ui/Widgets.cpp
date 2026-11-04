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

HeaderLayout headerLayout(int x, int y, int width, int height) {
    const int button = height + 8;
    return { { x, y, width, height }, { x, y, button, height }, { x + width - button, y, button, height } };
}

void drawHeader(Canvas& canvas, Font& font, const UiTheme& theme, const HeaderLayout& header, const char* title, bool hasSelect) {
    // Drawn as lines, so they need no glyphs in the font
    const auto backX = static_cast<float>(header.back.x + header.back.width / 2);
    const auto centerY = static_cast<float>(header.bar.y + header.bar.height / 2 - 1);
    canvas.line(backX + 3.0f, centerY - 6.0f, backX - 3.0f, centerY, 2.0f, theme.text);
    canvas.line(backX - 3.0f, centerY, backX + 3.0f, centerY + 6.0f, 2.0f, theme.text);
    const int titleX = header.back.x + header.back.width;
    const int titleRight = hasSelect ? header.select.x : header.bar.x + header.bar.width - PADDING;
    drawClippedText(canvas, font, titleX, header.bar.y + (header.bar.height - font.lineHeight()) / 2 - 1, title, titleRight - titleX, theme.text);
    if (hasSelect) {
        const auto checkX = static_cast<float>(header.select.x + header.select.width / 2);
        canvas.line(checkX - 6.0f, centerY, checkX - 2.0f, centerY + 4.0f, 2.0f, theme.accent);
        canvas.line(checkX - 2.0f, centerY + 4.0f, checkX + 6.0f, centerY - 5.0f, 2.0f, theme.accent);
    }
    canvas.fillRect(header.bar.x, header.bar.y + header.bar.height - 1, header.bar.width, 1, theme.border);
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

void ListView::scrollBy(int rows) {
    firstVisible = std::clamp(firstVisible + rows, 0, std::max(0, itemCount - visibleRows));
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
    // Header on top, hint at the bottom, rows in between
    listTop = font.lineHeight() + PADDING + 4;
    header = headerLayout(0, 0, width, listTop - 2);
    const int hintHeight = hint[0] != '\0' ? hintLines() * font.lineHeight() + 4 : 0;
    visibleRows = std::max(1, (height - listTop - hintHeight) / rowPixels);
    keepVisible();
}

void ListView::layoutPopup(int width, int height, Font& font, int popupWidth) {
    fullScreen = false;
    rowPixels = font.lineHeight() + ROW_PADDING;
    const int headerHeight = font.lineHeight() + PADDING;
    const int maxRows = std::max(1, (height - 2 * PADDING - headerHeight) / rowPixels);
    visibleRows = std::min(std::max(1, itemCount), maxRows);
    const int popupHeight = headerHeight + visibleRows * rowPixels + PADDING / 2 + 2;
    frame = { (width - popupWidth) / 2, (height - popupHeight) / 2, popupWidth, popupHeight };
    header = headerLayout(frame.x, frame.y, popupWidth, headerHeight);
    listTop = headerHeight + 2;
    keepVisible();
}

void ListView::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    if (fullScreen) {
        canvas.fillRect(frame.x, frame.y, frame.width, frame.height, theme.panel);
    } else {
        // The map stays visible but dimmed behind a popup
        canvas.fillRect(0, 0, canvas.width, canvas.height, theme.scrim, 90);
        canvas.fillRoundRect(frame.x, frame.y, frame.width, frame.height, 8, theme.panel, theme.panelOpacity);
    }
    drawHeader(canvas, font, theme, header, title, itemCount > 0 && highlighted);
    if (itemCount == 0 && hint[0] != '\0' && fullScreen) {
        return;
    }
    for (int row = 0; row < visibleRows && firstVisible + row < itemCount; row++) {
        const int index = firstVisible + row;
        const Item& item = items[index];
        const int y = frame.y + listTop + row * rowPixels;
        const bool isHighlighted = highlighted && index == selection;
        const int left = frame.x + 4;
        const int width = frame.width - 8;
        if (isHighlighted) {
            canvas.fillRoundRect(left, y, width, rowPixels - 2, 5, theme.accent);
        }
        const uint16_t color = isHighlighted ? theme.accentText : theme.text;
        const int textY = y + (rowPixels - 2 - font.lineHeight()) / 2;
        const int detailWidth = item.detail[0] != '\0' ? font.textWidth(item.detail) : 0;
        drawClippedText(canvas, font, left + PADDING, textY, item.label, width - 2 * PADDING - detailWidth - (detailWidth > 0 ? PADDING : 0), color);
        if (detailWidth > 0) {
            font.drawText(canvas, left + width - PADDING - detailWidth, textY, item.detail, isHighlighted ? theme.accentText : theme.muted);
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
    if (!highlighted) {
        // The first step shows the highlight where the list is, it does not move yet
        highlighted = true;
        if (selection < firstVisible || selection >= firstVisible + visibleRows) {
            selection = firstVisible;
        }
        return UiAction::Moved;
    }
    const int previous = selection;
    select(selection + rows);
    return selection != previous ? UiAction::Moved : UiAction::None;
}

UiAction ListView::handleKey(uint32_t key) {
    if (!highlighted && (key == keys::ENTER || key == ' ' || key == keys::YES)) {
        return move(1);
    }
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
    if (header.back.contains(x, y)) {
        return UiAction::Back;
    }
    if (header.select.contains(x, y)) {
        return itemCount > 0 && highlighted ? UiAction::Chosen : UiAction::None;
    }
    const int index = rowAt(x, y);
    if (index >= 0) {
        select(index);
        return UiAction::Chosen;
    }
    // A popup also closes when tapped outside
    if (!fullScreen && !frame.contains(x, y)) {
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

void TextPrompt::open(const char* title, const char* text) {
    snprintf(heading, sizeof(heading), "%s", title);
    snprintf(value, sizeof(value), "%s", text);
    visible = true;
}

void TextPrompt::layout(int width, int height, Font& font) {
    const int dialogWidth = std::min(width - 16, 280);
    const int line = font.lineHeight();
    const int buttonHeight = line + 12;
    const int dialogHeight = PADDING + line + PADDING / 2 + line + 8 + PADDING + buttonHeight + PADDING;
    // In the upper part of the screen, an on-screen keyboard may come later below it
    frame = { (width - dialogWidth) / 2, std::max(4, (height - dialogHeight) / 3), dialogWidth, dialogHeight };
    field = { frame.x + PADDING, frame.y + PADDING + line + PADDING / 2, dialogWidth - 2 * PADDING, line + 8 };
    const int buttonWidth = (dialogWidth - 3 * PADDING) / 2;
    const int buttonY = frame.y + dialogHeight - PADDING - buttonHeight;
    cancelButton = { frame.x + PADDING, buttonY, buttonWidth, buttonHeight };
    okButton = { frame.x + 2 * PADDING + buttonWidth, buttonY, buttonWidth, buttonHeight };
}

void TextPrompt::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    canvas.fillRect(0, 0, canvas.width, canvas.height, theme.scrim, 110);
    canvas.fillRoundRect(frame.x, frame.y, frame.width, frame.height, 8, theme.panel);
    font.drawText(canvas, frame.x + PADDING, frame.y + PADDING, heading, theme.text);
    canvas.fillRoundRect(field.x, field.y, field.width, field.height, 4, theme.border);
    canvas.fillRoundRect(field.x + 1, field.y + 1, field.width - 2, field.height - 2, 3, theme.panel);
    // The end of a long name stays in view, with the caret after it
    const char* shown = value;
    while (*shown != '\0' && font.textWidth(shown) > field.width - 12) {
        shown++;
        while ((static_cast<unsigned char>(*shown) & 0xC0) == 0x80) {
            shown++;
        }
    }
    font.drawText(canvas, field.x + 4, field.y + 4, shown, theme.text);
    const int caretX = field.x + 4 + font.textWidth(shown) + 1;
    canvas.fillRect(caretX, field.y + 3, 2, field.height - 6, theme.accent, 255);
    drawButton(canvas, font, theme, cancelButton, "Cancel", false);
    drawButton(canvas, font, theme, okButton, "OK", true);
}

void TextPrompt::append(uint32_t codepoint) {
    char bytes[4];
    int count = 0;
    if (codepoint < 0x80) {
        bytes[count++] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        bytes[count++] = static_cast<char>(0xC0 | (codepoint >> 6));
        bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        bytes[count++] = static_cast<char>(0xE0 | (codepoint >> 12));
        bytes[count++] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        bytes[count++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    const size_t length = strlen(value);
    if (length + static_cast<size_t>(count) < sizeof(value)) {
        memcpy(value + length, bytes, static_cast<size_t>(count));
        value[length + static_cast<size_t>(count)] = '\0';
    }
}

void TextPrompt::erase() {
    size_t length = strlen(value);
    // A whole UTF-8 character
    while (length > 0 && (static_cast<unsigned char>(value[length - 1]) & 0xC0) == 0x80) {
        length--;
    }
    if (length > 0) {
        length--;
    }
    value[length] = '\0';
}

UiAction TextPrompt::handleKey(uint32_t key) {
    switch (key) {
        case keys::ENTER:
        case keys::YES:
            visible = false;
            return UiAction::Chosen;
        case keys::ESCAPE:
        case keys::NO:
            visible = false;
            return UiAction::Back;
        case keys::BACKSPACE:
        case keys::DELETE:
            erase();
            return UiAction::Moved;
        default:
            break;
    }
    // Printable characters only; arrows, focus keys and the like are above U+2000 or below space
    if ((key >= 0x20 && key < 0x7F) || (key >= 0xA0 && key < 0x2000)) {
        append(key);
        return UiAction::Moved;
    }
    return UiAction::None;
}

UiAction TextPrompt::handleTap(int x, int y) {
    if (okButton.contains(x, y)) {
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
