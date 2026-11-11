// SPDX-License-Identifier: GPL-3.0-or-later
#include "WaypointSheet.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr int PADDING = 8;

const char* labelOf(WaypointSheet::Button button) {
    switch (button) {
        case WaypointSheet::Button::Save:
            return "Save";
        case WaypointSheet::Button::Cancel:
            return "Cancel";
        case WaypointSheet::Button::Rename:
            return "Rename";
        case WaypointSheet::Button::Delete:
            return "Delete";
        case WaypointSheet::Button::Close:
            return "Close";
        case WaypointSheet::Button::SaveAsWaypoint:
            return "Save";
        case WaypointSheet::Button::Navigate:
            return "Go";
        default:
            return "";
    }
}

} // namespace

void WaypointSheet::openNew(const char* defaultName, const char* coordinates, const char* distance, bool canEdit) {
    kind = Mode::New;
    editable = canEdit;
    selectedAll = canEdit;
    snprintf(value, sizeof(value), "%s", defaultName);
    snprintf(coordinateText, sizeof(coordinateText), "%s", coordinates);
    snprintf(distanceText, sizeof(distanceText), "%s", distance);
    focus = 0;
    visible = true;
}

void WaypointSheet::openExisting(const char* name, const char* coordinates, const char* distance) {
    kind = Mode::Existing;
    editable = false;
    selectedAll = false;
    snprintf(value, sizeof(value), "%s", name);
    snprintf(coordinateText, sizeof(coordinateText), "%s", coordinates);
    snprintf(distanceText, sizeof(distanceText), "%s", distance);
    // Close first, so a stray Enter changes nothing
    focus = 3;
    visible = true;
}

void WaypointSheet::openPlace(const char* name, const char* type, const char* coordinates, const char* distance) {
    kind = Mode::Place;
    editable = false;
    selectedAll = false;
    snprintf(value, sizeof(value), "%s", name);
    snprintf(typeText, sizeof(typeText), "%s", type);
    snprintf(coordinateText, sizeof(coordinateText), "%s", coordinates);
    snprintf(distanceText, sizeof(distanceText), "%s", distance);
    // Close first, so pressing Enter again on the map's next item does not save anything by mistake
    focus = 2;
    visible = true;
}

void WaypointSheet::setDistance(const char* distance) {
    snprintf(distanceText, sizeof(distanceText), "%s", distance);
}

WaypointSheet::Button WaypointSheet::buttonAt(int index) const {
    // "Go" is "Navigate here": a route to the point, from the position or the screen centre
    static constexpr Button NEW_BUTTONS[] = { Button::Save, Button::Navigate, Button::Cancel };
    static constexpr Button EXISTING_BUTTONS[] = { Button::Navigate, Button::Rename, Button::Delete, Button::Close };
    static constexpr Button PLACE_BUTTONS[] = { Button::Navigate, Button::SaveAsWaypoint, Button::Close };
    if (index < 0 || index >= buttonCount()) {
        return Button::None;
    }
    if (kind == Mode::Place) {
        return PLACE_BUTTONS[index];
    }
    return kind == Mode::New ? NEW_BUTTONS[index] : EXISTING_BUTTONS[index];
}

void WaypointSheet::layout(int width, int height, Font& font, bool compact) {
    compactLayout = compact;
    const int line = font.lineHeight();
    const int sheetWidth = std::min(width - 8, 300);
    const int buttonHeight = line + 10;
    const int sheetHeight = PADDING + (line + 8) + 4 + (compact ? 0 : textLines() * line) + (compact ? 0 : PADDING) + buttonHeight + PADDING;
    frame = { (width - sheetWidth) / 2, height - sheetHeight - 4, sheetWidth, sheetHeight };
    // The < at the left of the name goes back, like in the lists
    back = { frame.x, frame.y, line + 16, line + 8 + PADDING };
    field = { frame.x + back.width, frame.y + PADDING / 2 + 2, sheetWidth - back.width - PADDING, line + 8 };
    const int count = buttonCount();
    const int buttonWidth = (sheetWidth - (count + 1) * PADDING) / count;
    const int buttonY = frame.y + sheetHeight - PADDING - buttonHeight;
    for (int i = 0; i < count; i++) {
        buttons[i] = { frame.x + PADDING + i * (buttonWidth + PADDING), buttonY, buttonWidth, buttonHeight };
    }
}

void WaypointSheet::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    const int line = font.lineHeight();
    canvas.fillRoundRect(frame.x, frame.y, frame.width, frame.height, 8, theme.panel, theme.panelOpacity);
    const auto backX = static_cast<float>(back.x + back.width / 2);
    const auto backY = static_cast<float>(field.y + field.height / 2);
    canvas.line(backX + 3.0f, backY - 6.0f, backX - 3.0f, backY, 2.0f, theme.text);
    canvas.line(backX - 3.0f, backY, backX + 3.0f, backY + 6.0f, 2.0f, theme.text);
    if (editable) {
        canvas.fillRoundRect(field.x, field.y, field.width, field.height, 4, theme.border);
        canvas.fillRoundRect(field.x + 1, field.y + 1, field.width - 2, field.height - 2, 3, theme.panel);
        // The end of a long name stays in view
        const char* shown = value;
        while (*shown != '\0' && font.textWidth(shown) > field.width - 12) {
            shown++;
            while ((static_cast<unsigned char>(*shown) & 0xC0) == 0x80) {
                shown++;
            }
        }
        const int textWidth = font.textWidth(shown);
        if (selectedAll && textWidth > 0) {
            canvas.fillRect(field.x + 3, field.y + 3, textWidth + 2, field.height - 6, theme.accent);
            font.drawText(canvas, field.x + 4, field.y + 4, shown, theme.accentText);
        } else {
            font.drawText(canvas, field.x + 4, field.y + 4, shown, theme.text);
            canvas.fillRect(field.x + 4 + textWidth + 1, field.y + 3, 2, field.height - 6, theme.accent, 255);
        }
    } else {
        drawClippedText(canvas, font, field.x + 4, field.y + 4, value, field.width - 8, theme.text);
    }
    int textY = field.y + field.height + 4;
    if (compactLayout) {
        textY = frame.y + frame.height;  // no detail lines: the keyboard needs the room
    } else if (kind == Mode::Place) {
        drawClippedText(canvas, font, frame.x + PADDING, textY, typeText, frame.width - 2 * PADDING, theme.text);
        textY += line;
    }
    if (!compactLayout) {
        drawClippedText(canvas, font, frame.x + PADDING, textY, coordinateText, frame.width - 2 * PADDING, theme.muted);
        drawClippedText(canvas, font, frame.x + PADDING, textY + line, distanceText, frame.width - 2 * PADDING, theme.muted);
    }
    for (int i = 0; i < buttonCount(); i++) {
        drawButton(canvas, font, theme, buttons[i], labelOf(buttonAt(i)), i == focus, buttonAt(i) == Button::Delete);
    }
}

void WaypointSheet::append(uint32_t codepoint) {
    if (selectedAll) {
        value[0] = '\0';
        selectedAll = false;
    }
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

void WaypointSheet::erase() {
    if (selectedAll) {
        value[0] = '\0';
        selectedAll = false;
        return;
    }
    size_t length = strlen(value);
    while (length > 0 && (static_cast<unsigned char>(value[length - 1]) & 0xC0) == 0x80) {
        length--;
    }
    if (length > 0) {
        length--;
    }
    value[length] = '\0';
}

WaypointSheet::Button WaypointSheet::handleKey(uint32_t key) {
    switch (key) {
        case keys::ENTER:
        case keys::YES:
            return buttonAt(focus);
        case keys::ESCAPE:
        case keys::NO:
            return kind == Mode::New ? Button::Cancel : Button::Close;
        case keys::TAB:
        case keys::RIGHT:
        case keys::DOWN:
        case keys::FOCUS_NEXT:
            focus = (focus + 1) % buttonCount();
            return Button::None;
        case keys::LEFT:
        case keys::UP:
        case keys::FOCUS_PREVIOUS:
            focus = (focus + buttonCount() - 1) % buttonCount();
            return Button::None;
        default:
            break;
    }
    if (kind == Mode::Place) {
        if (key == keys::BACKSPACE) {
            return Button::Close;
        }
        if (key == 'n' || key == 'N' || key == 'g' || key == 'G') {
            return Button::Navigate;
        }
        return key == 's' || key == 'S' ? Button::SaveAsWaypoint : Button::None;
    }
    if (kind == Mode::Existing) {
        // Without a text field Backspace is back, and letters are shortcuts
        if (key == keys::BACKSPACE) {
            return Button::Close;
        }
        if (key == 'r' || key == 'R') {
            return Button::Rename;
        }
        if (key == 'x' || key == 'X' || key == keys::DELETE) {
            return Button::Delete;
        }
        if (key == 'n' || key == 'N' || key == 'g' || key == 'G') {
            return Button::Navigate;
        }
        return Button::None;
    }
    if (!editable) {
        return key == keys::BACKSPACE ? Button::Cancel : Button::None;
    }
    if (key == keys::BACKSPACE || key == keys::DELETE) {
        erase();
        return Button::None;
    }
    if ((key >= 0x20 && key < 0x7F) || (key >= 0xA0 && key < 0x2000)) {
        append(key);
    }
    return Button::None;
}

WaypointSheet::Button WaypointSheet::handleTap(int x, int y) {
    for (int i = 0; i < buttonCount(); i++) {
        if (buttons[i].contains(x, y)) {
            return buttonAt(i);
        }
    }
    if (!frame.contains(x, y) || back.contains(x, y)) {
        return kind == Mode::New ? Button::Cancel : Button::Close;
    }
    return Button::None;
}

} // namespace maps
