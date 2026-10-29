// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.h"

#include <algorithm>

namespace maps {

void Controls::layout(int width, int height, int scaleBarTop) {
    // The zoom pair sits on the right edge around the middle, the position button above the scale bar
    rects[static_cast<int>(Control::Menu)] = { EDGE, EDGE, SIZE, SIZE };
    rects[static_cast<int>(Control::ZoomIn)] = { width - EDGE - SIZE, height / 2 - SIZE - 3, SIZE, SIZE };
    rects[static_cast<int>(Control::ZoomOut)] = { width - EDGE - SIZE, height / 2 + 3, SIZE, SIZE };
    const int positionTop = std::max(height / 2 + SIZE + 12, scaleBarTop - EDGE - SIZE);
    rects[static_cast<int>(Control::Position)] = { width - EDGE - SIZE, positionTop, SIZE, SIZE };
}

void Controls::show() {
    visible = true;
    focus = Control::Menu;
}

void Controls::toggle() {
    if (visible) {
        hide();
    } else {
        show();
    }
}

Control Controls::hit(int x, int y) const {
    if (!visible) {
        return Control::None;
    }
    // A few pixels of slack around each button, fingers are wider than 32 px
    for (int i = 0; i < static_cast<int>(Control::Count); i++) {
        const Rect& r = rects[i];
        if (x >= r.x - 4 && y >= r.y - 3 && x < r.x + r.width + 4 && y < r.y + r.height + 3) {
            return static_cast<Control>(i);
        }
    }
    return Control::None;
}

char Controls::keyOf(Control control) {
    switch (control) {
        case Control::Menu:
            return 'M';
        case Control::ZoomIn:
            return 'I';
        case Control::ZoomOut:
            return 'O';
        case Control::Position:
            return 'G';
        default:
            return 0;
    }
}

Control Controls::controlForKey(uint32_t key) {
    switch (key) {
        case 'm':
        case 'M':
            return Control::Menu;
        case 'i':
        case 'I':
            return Control::ZoomIn;
        case 'o':
        case 'O':
            return Control::ZoomOut;
        case 'g':
        case 'G':
            return Control::Position;
        default:
            return Control::None;
    }
}

void Controls::moveFocus(int steps) {
    const int count = static_cast<int>(Control::Count);
    const int next = ((static_cast<int>(focus) + steps) % count + count) % count;
    focus = static_cast<Control>(next);
}

void Controls::drawIcon(Canvas& canvas, Control control, const Rect& r, uint16_t color, PositionButton position, const UiTheme& theme) {
    const float cx = static_cast<float>(r.x) + r.width / 2.0f;
    const float cy = static_cast<float>(r.y) + r.height / 2.0f;
    switch (control) {
        case Control::Menu:
            for (int i = -1; i <= 1; i++) {
                canvas.line(cx - 7.0f, cy + i * 5.0f, cx + 7.0f, cy + i * 5.0f, 2.2f, color);
            }
            break;
        case Control::ZoomIn:
            canvas.line(cx - 7.0f, cy, cx + 7.0f, cy, 2.4f, color);
            canvas.line(cx, cy - 7.0f, cx, cy + 7.0f, 2.4f, color);
            break;
        case Control::ZoomOut:
            canvas.line(cx - 7.0f, cy, cx + 7.0f, cy, 2.4f, color);
            break;
        case Control::Position:
            canvas.ring(cx, cy, 6.0f, 2.0f, color);
            canvas.line(cx, cy - 11.0f, cx, cy - 7.0f, 2.0f, color);
            canvas.line(cx, cy + 7.0f, cx, cy + 11.0f, 2.0f, color);
            canvas.line(cx - 11.0f, cy, cx - 7.0f, cy, 2.0f, color);
            canvas.line(cx + 7.0f, cy, cx + 11.0f, cy, 2.0f, color);
            if (position == PositionButton::Following) {
                canvas.fillCircle(cx, cy, 3.0f, color);
            } else if (position == PositionButton::Unavailable) {
                // Crossed out, with a gap in the panel colour so the slash reads on the icon
                canvas.line(cx - 10.0f, cy + 10.0f, cx + 10.0f, cy - 10.0f, 4.5f, theme.panel);
                canvas.line(cx - 10.0f, cy + 10.0f, cx + 10.0f, cy - 10.0f, 2.0f, color);
            }
            break;
        default:
            break;
    }
}

void Controls::draw(Canvas& canvas, Font& font, const UiTheme& theme, PositionButton position) {
    if (!visible) {
        return;
    }
    const bool hints = inputs.keyboard;
    for (int i = 0; i < static_cast<int>(Control::Count); i++) {
        const auto control = static_cast<Control>(i);
        const Rect& r = rects[i];
        const bool filled = control == Control::Position && position == PositionButton::Following;
        const bool focusedNow = usesFocus() && control == focus;
        canvas.fillRoundRect(r.x, r.y, r.width, r.height, 7, filled ? theme.accent : theme.panel, filled ? 255 : theme.controlOpacity);
        if (focusedNow) {
            canvas.ring(r.x + r.width / 2.0f, r.y + r.height / 2.0f, r.width / 2.0f + 2.0f, 2.0f, theme.accent);
        }
        uint16_t color = filled ? theme.accentText : theme.text;
        if (control == Control::Position && position == PositionButton::Unavailable) {
            color = theme.muted;
        }
        drawIcon(canvas, control, r, color, position, theme);
        if (!hints) {
            continue;
        }
        // The key in a small tag beside the button, towards the screen centre
        const char label[2] = { keyOf(control), '\0' };
        const int tagWidth = font.textWidth(label) + 6;
        const int tagHeight = font.lineHeight();
        const bool rightSide = r.x > canvas.width / 2;
        const int tagX = rightSide ? r.x - tagWidth - 3 : r.x + r.width + 3;
        const int tagY = r.y + (r.height - tagHeight) / 2;
        canvas.fillRoundRect(tagX, tagY, tagWidth, tagHeight, 4, theme.text, theme.controlOpacity);
        font.drawText(canvas, tagX + 3, tagY, label, theme.panel);
    }
}

// ---------------------------------------------------------------------------------------------------------------

void Attribution::expand(uint32_t now) {
    expanded = true;
    until = now + SHOW_MS;
}

bool Attribution::update(uint32_t now) {
    if (expanded && static_cast<int32_t>(now - until) >= 0) {
        expanded = false;
        return true;
    }
    return false;
}

Attribution::Tap Attribution::tap(int x, int y, const Rect& bar, const Rect& mark, uint32_t now) {
    if (expanded) {
        if (bar.contains(x, y)) {
            return Tap::OpenAbout;
        }
        return Tap::Missed;
    }
    // The mark answers to a little more than its size, so a finger finds it
    const Rect target { mark.x, mark.y - 12, mark.width + 12, mark.height + 16 };
    if (target.contains(x, y)) {
        expand(now);
        return Tap::Expanded;
    }
    return Tap::Missed;
}

} // namespace maps
