// SPDX-License-Identifier: GPL-3.0-or-later
#include "RouteCard.h"

#include <algorithm>
#include <cstdio>

namespace maps {

namespace {

constexpr int PADDING = 8;
/** The mode bar: segments of this size, this far below the top of the screen. */
constexpr int MODE_WIDTH = 52;
constexpr int MODE_HEIGHT = 30;
constexpr int MODE_TOP = 4;

const char* labelOf(RouteCard::Button button) {
    switch (button) {
        case RouteCard::Button::Start:
            return "Start";
        case RouteCard::Button::Close:
            return "Close";
        default:
            return "";
    }
}

} // namespace

void drawModeIcon(Canvas& canvas, int modeIndex, float cx, float cy, uint16_t color) {
    switch (modeIndex) {
        case 1:
            // A bicycle: two wheels, the frame between them, the handlebar
            canvas.ring(cx - 6.5f, cy + 3.0f, 4.5f, 1.6f, color);
            canvas.ring(cx + 6.5f, cy + 3.0f, 4.5f, 1.6f, color);
            canvas.line(cx - 6.5f, cy + 3.0f, cx - 1.5f, cy - 4.0f, 1.6f, color);
            canvas.line(cx - 1.5f, cy - 4.0f, cx + 4.5f, cy - 4.0f, 1.6f, color);
            canvas.line(cx + 4.5f, cy - 4.0f, cx + 6.5f, cy + 3.0f, 1.6f, color);
            canvas.line(cx - 6.5f, cy + 3.0f, cx + 0.5f, cy + 3.0f, 1.6f, color);
            canvas.line(cx + 0.5f, cy + 3.0f, cx + 4.5f, cy - 4.0f, 1.6f, color);
            canvas.line(cx + 3.5f, cy - 6.5f, cx + 6.0f, cy - 6.5f, 1.6f, color);
            break;
        case 2:
            // A car from the side: body, cabin, two wheels
            canvas.fillRoundRect(static_cast<int>(cx - 10.0f), static_cast<int>(cy - 1.0f), 20, 6, 2, color);
            canvas.line(cx - 6.0f, cy - 1.0f, cx - 3.5f, cy - 6.0f, 1.8f, color);
            canvas.line(cx - 3.5f, cy - 6.0f, cx + 4.0f, cy - 6.0f, 1.8f, color);
            canvas.line(cx + 4.0f, cy - 6.0f, cx + 7.0f, cy - 1.0f, 1.8f, color);
            canvas.fillCircle(cx - 5.5f, cy + 6.0f, 2.6f, color);
            canvas.fillCircle(cx + 5.5f, cy + 6.0f, 2.6f, color);
            break;
        default:
            // A walking figure: head, body, arms and legs in a stride
            canvas.fillCircle(cx + 1.0f, cy - 7.0f, 2.4f, color);
            canvas.line(cx + 0.5f, cy - 3.5f, cx - 0.5f, cy + 2.5f, 2.0f, color);
            canvas.line(cx - 0.5f, cy + 2.5f, cx - 4.0f, cy + 8.5f, 2.0f, color);
            canvas.line(cx - 0.5f, cy + 2.5f, cx + 3.5f, cy + 8.5f, 2.0f, color);
            canvas.line(cx + 0.3f, cy - 2.5f, cx - 4.0f, cy + 1.0f, 1.8f, color);
            canvas.line(cx + 0.3f, cy - 2.5f, cx + 4.5f, cy, 1.8f, color);
            break;
    }
}

void RouteCard::open(const char* summary, const char* note) {
    setSummary(summary);
    setNote(note);
    busy = false;
    canStart = true;
    // Opened again for a new search or its result: the focus stays where it was (on a mode the user is trying)
    if (!visible) {
        focus = FOCUS_START;
    }
    visible = true;
}

void RouteCard::setSummary(const char* summary) {
    snprintf(summaryText, sizeof(summaryText), "%s", summary != nullptr ? summary : "");
}

void RouteCard::setNote(const char* note) {
    snprintf(noteText, sizeof(noteText), "%s", note != nullptr ? note : "");
    // Wrapped again by the next layout()
    noteLineCount = 0;
}

RouteCard::Button RouteCard::buttonAt(int index) {
    static constexpr Button ORDER[FOCUS_COUNT] = { Button::Walk, Button::Bike, Button::Car, Button::Start, Button::Close };
    return index >= 0 && index < FOCUS_COUNT ? ORDER[index] : Button::None;
}

RouteCard::Button RouteCard::pressed(Button button) const {
    return button == Button::Start && (busy || !canStart) ? Button::None : button;
}

void RouteCard::layout(int width, int height, Font& font) {
    const int line = font.lineHeight();
    const int cardWidth = std::min(width - 8, 300);
    noteLineCount = 0;
    if (noteText[0] != '\0') {
        noteLineCount = wrapText(font, noteText, cardWidth - 2 * PADDING, &noteLines[0][0], NOTE_LINE_LENGTH, NOTE_LINES);
    }
    const int buttonHeight = line + 10;
    const int cardHeight = PADDING + line + 4 + noteLineCount * line + (noteLineCount > 0 ? 4 : 0) + buttonHeight + PADDING;
    frame = { (width - cardWidth) / 2, height - cardHeight - 4, cardWidth, cardHeight };
    const int buttonWidth = (cardWidth - 3 * PADDING) / 2;
    const int buttonY = frame.y + cardHeight - PADDING - buttonHeight;
    for (int i = 0; i < 2; i++) {
        buttons[i] = { frame.x + PADDING + i * (buttonWidth + PADDING), buttonY, buttonWidth, buttonHeight };
    }
    const int barWidth = MODE_COUNT * MODE_WIDTH + (MODE_COUNT + 1) * 4;
    modeBar = { (width - barWidth) / 2, MODE_TOP, barWidth, MODE_HEIGHT + 8 };
    for (int i = 0; i < MODE_COUNT; i++) {
        modes[i] = { modeBar.x + 4 + i * (MODE_WIDTH + 4), modeBar.y + 4, MODE_WIDTH, MODE_HEIGHT };
    }
}

void RouteCard::draw(Canvas& canvas, Font& font, const UiTheme& theme) {
    // The modes: the chosen one filled, the focused one framed
    canvas.fillRoundRect(modeBar.x, modeBar.y, modeBar.width, modeBar.height, 8, theme.panel, theme.panelOpacity);
    for (int i = 0; i < MODE_COUNT; i++) {
        const Rect& r = modes[i];
        const bool chosen = i == mode;
        if (chosen) {
            canvas.fillRoundRect(r.x, r.y, r.width, r.height, 6, theme.accent);
        }
        if (focus == i) {
            canvas.frameRect(r.x - 1, r.y - 1, r.width + 2, r.height + 2, chosen ? theme.text : theme.accent, 2);
        }
        drawModeIcon(canvas, i, static_cast<float>(r.x) + r.width / 2.0f, static_cast<float>(r.y) + r.height / 2.0f, chosen ? theme.accentText : theme.text);
    }
    const int line = font.lineHeight();
    const int textWidth = frame.width - 2 * PADDING;
    canvas.fillRoundRect(frame.x, frame.y, frame.width, frame.height, 8, theme.panel, theme.panelOpacity);
    int textY = frame.y + PADDING;
    drawClippedText(canvas, font, frame.x + PADDING, textY, busy ? "Finding a route..." : summaryText, textWidth, theme.text);
    textY += line + 4;
    for (int i = 0; i < noteLineCount; i++) {
        drawClippedText(canvas, font, frame.x + PADDING, textY, noteLines[i], textWidth, theme.muted);
        textY += line;
    }
    for (int i = 0; i < 2; i++) {
        const Button button = buttonAt(FOCUS_START + i);
        // Start without a route to start is drawn muted, framed when it has the focus
        if (button == Button::Start && (busy || !canStart)) {
            const bool focused = focus == FOCUS_START;
            canvas.fillRoundRect(buttons[i].x, buttons[i].y, buttons[i].width, buttons[i].height, 6, focused ? theme.accent : theme.border);
            canvas.fillRoundRect(buttons[i].x + (focused ? 2 : 1), buttons[i].y + (focused ? 2 : 1), buttons[i].width - (focused ? 4 : 2),
                buttons[i].height - (focused ? 4 : 2), 5, theme.panel);
            const int labelWidth = font.textWidth(labelOf(button));
            font.drawText(canvas, buttons[i].x + (buttons[i].width - labelWidth) / 2, buttons[i].y + (buttons[i].height - line) / 2, labelOf(button),
                theme.muted);
            continue;
        }
        drawButton(canvas, font, theme, buttons[i], labelOf(button), FOCUS_START + i == focus);
    }
}

RouteCard::Button RouteCard::handleKey(uint32_t key) {
    switch (key) {
        case keys::ENTER:
        case keys::YES:
            return pressed(buttonAt(focus));
        case keys::ESCAPE:
        case keys::NO:
        case keys::BACKSPACE:
            return Button::Close;
        case keys::TAB:
        case keys::RIGHT:
        case keys::DOWN:
        case keys::FOCUS_NEXT:
            focus = (focus + 1) % FOCUS_COUNT;
            return Button::None;
        case keys::LEFT:
        case keys::UP:
        case keys::FOCUS_PREVIOUS:
            focus = (focus + FOCUS_COUNT - 1) % FOCUS_COUNT;
            return Button::None;
        case 's':
        case 'S':
            return pressed(Button::Start);
        case 'p':
        case 'P':
            return Button::Profile;
        case 'w':
        case 'W':
        case '1':
            return Button::Walk;
        case 'b':
        case 'B':
        case '2':
            return Button::Bike;
        case 'c':
        case 'C':
        case '3':
            return Button::Car;
        default:
            return Button::None;
    }
}

RouteCard::Button RouteCard::handleTap(int x, int y) {
    for (int i = 0; i < MODE_COUNT; i++) {
        if (modes[i].contains(x, y)) {
            return buttonAt(i);
        }
    }
    for (int i = 0; i < 2; i++) {
        if (buttons[i].contains(x, y)) {
            return pressed(buttonAt(FOCUS_START + i));
        }
    }
    return contains(x, y) ? Button::None : Button::Close;
}

} // namespace maps
