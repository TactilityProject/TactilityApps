// SPDX-License-Identifier: GPL-3.0-or-later
#include "Controls.h"

#include "Metrics.h"

#include <algorithm>

namespace maps {

int Controls::sizeFor(int width, int height) {
    const int shortSide = std::min(width, height);
    int result = std::clamp(shortSide / 10, SIZE, MAX_SIZE);
    // A low screen: the zoom pair, the position button and the menu pair must fit one above the other
    if (height < 200) {
        result = std::max(20, std::min(result, height / 6));
    } else if (metrics::touchTargets()) {
        // A finger-sized button on touch boards
        result = std::max(result, metrics::TOUCH_BUTTON_HEIGHT);
    }
    return result;
}

void Controls::layout(int width, int height, int scaleBarTop, int top) {
    // The zoom pair sits on the right edge around the middle, the position button above the scale bar
    size = sizeFor(width, height);
    const int SIZE = size;
    rects[static_cast<int>(Control::Menu)] = { EDGE, top + EDGE, SIZE, SIZE };
    rects[static_cast<int>(Control::ZoomIn)] = { width - EDGE - SIZE, height / 2 - SIZE - 3, SIZE, SIZE };
    rects[static_cast<int>(Control::ZoomOut)] = { width - EDGE - SIZE, height / 2 + 3, SIZE, SIZE };
    rects[static_cast<int>(Control::Search)] = { EDGE, top + EDGE + SIZE + EDGE, SIZE, SIZE };
    rects[static_cast<int>(Control::Style)] = styleButton ? Rect { EDGE, top + EDGE + 2 * (SIZE + EDGE), SIZE, SIZE } : Rect {};
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
        if (r.width == 0) {
            continue;
        }
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
        case Control::Search:
            return '/';
        case Control::Style:
            return 'L';
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
        case '/':
            return Control::Search;
        case 'l':
        case 'L':
            return Control::Style;
        default:
            return Control::None;
    }
}

void Controls::moveFocus(int steps) {
    const int count = static_cast<int>(Control::Count);
    int next = ((static_cast<int>(focus) + steps) % count + count) % count;
    if (!styleButton && static_cast<Control>(next) == Control::Style) {
        // Not shown: skipped
        next = ((next + (steps < 0 ? -1 : 1)) % count + count) % count;
    }
    focus = static_cast<Control>(next);
}

void Controls::drawIcon(Canvas& canvas, Control control, const Rect& r, uint16_t color, PositionButton position, const UiTheme& theme) {
    const float cx = static_cast<float>(r.x) + r.width / 2.0f;
    const float cy = static_cast<float>(r.y) + r.height / 2.0f;
    // The icons are drawn for 32 px buttons and scaled to the real size
    const float k = static_cast<float>(r.width) / 32.0f;
    switch (control) {
        case Control::Menu:
            for (int i = -1; i <= 1; i++) {
                canvas.line(cx - 7.0f * k, cy + i * 5.0f * k, cx + 7.0f * k, cy + i * 5.0f * k, 2.2f * k, color);
            }
            break;
        case Control::ZoomIn:
            canvas.line(cx - 7.0f * k, cy, cx + 7.0f * k, cy, 2.4f * k, color);
            canvas.line(cx, cy - 7.0f * k, cx, cy + 7.0f * k, 2.4f * k, color);
            break;
        case Control::ZoomOut:
            canvas.line(cx - 7.0f * k, cy, cx + 7.0f * k, cy, 2.4f * k, color);
            break;
        case Control::Position:
            canvas.ring(cx, cy, 6.0f * k, 2.0f * k, color);
            canvas.line(cx, cy - 11.0f * k, cx, cy - 7.0f * k, 2.0f * k, color);
            canvas.line(cx, cy + 7.0f * k, cx, cy + 11.0f * k, 2.0f * k, color);
            canvas.line(cx - 11.0f * k, cy, cx - 7.0f * k, cy, 2.0f * k, color);
            canvas.line(cx + 7.0f * k, cy, cx + 11.0f * k, cy, 2.0f * k, color);
            if (position == PositionButton::Following) {
                canvas.fillCircle(cx, cy, 3.0f * k, color);
            } else if (position == PositionButton::FollowingHeading) {
                // A pointer to the top: the direction of travel is up
                canvas.line(cx, cy - 4.0f * k, cx - 3.0f * k, cy + 3.0f * k, 2.2f * k, color);
                canvas.line(cx, cy - 4.0f * k, cx + 3.0f * k, cy + 3.0f * k, 2.2f * k, color);
            } else if (position == PositionButton::Unavailable) {
                // Crossed out, with a gap in the panel colour so the slash reads on the icon
                canvas.line(cx - 10.0f * k, cy + 10.0f * k, cx + 10.0f * k, cy - 10.0f * k, 4.5f * k, theme.panel);
                canvas.line(cx - 10.0f * k, cy + 10.0f * k, cx + 10.0f * k, cy - 10.0f * k, 2.0f * k, color);
            }
            break;
        case Control::Search:
            // A magnifier: a lens ring and a handle towards the bottom right
            canvas.ring(cx - 2.0f * k, cy - 2.0f * k, 5.5f * k, 2.2f * k, color);
            canvas.line(cx + 2.0f * k, cy + 2.0f * k, cx + 8.0f * k, cy + 8.0f * k, 2.6f * k, color);
            break;
        case Control::Style: {
            // Layers: two flat diamonds, one above the other
            for (const float dy : { -3.0f, 3.0f }) {
                const float y = cy + dy * k;
                canvas.line(cx - 8.0f * k, y, cx, y - 4.0f * k, 2.0f * k, color);
                canvas.line(cx, y - 4.0f * k, cx + 8.0f * k, y, 2.0f * k, color);
                canvas.line(cx + 8.0f * k, y, cx, y + 4.0f * k, 2.0f * k, color);
                canvas.line(cx, y + 4.0f * k, cx - 8.0f * k, y, 2.0f * k, color);
            }
            break;
        }
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
        if (r.width == 0) {
            continue;
        }
        const bool filled = control == Control::Position && (position == PositionButton::Following || position == PositionButton::FollowingHeading);
        const bool focusedNow = (usesFocus() && control == focus) || control == hover;
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
