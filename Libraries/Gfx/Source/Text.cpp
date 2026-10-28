// SPDX-License-Identifier: Apache-2.0
#include <gfx/Surface.h>
#include <gfx/Text.h>

#include <algorithm>
#include <cstddef>

namespace gfx {

namespace {

struct LineSlice {
    const char* begin;
    const char* end;
    const char* next;
};

TextStyle normaliseStyle(const TextStyle& style) {
    TextStyle result = style;
    if (result.font == nullptr) {
        result.font = &FONT_5X7;
    }
    if (result.scale == 0) {
        result.scale = 1;
    }
    return result;
}

int glyphAdvance(const Font& font, int scale) {
    return static_cast<int>(font.advance) * scale;
}

/** Horizontal step for one character, including tabs. */
int advanceOfChar(char c, const Font& font, int scale, uint8_t tabSize) {
    if (c == '\r') {
        return 0;
    }
    if (c == '\t') {
        const int cells = tabSize == 0 ? 4 : tabSize;
        return glyphAdvance(font, scale) * cells;
    }
    return static_cast<int>(font.advanceFor(c)) * scale;
}

/** Width of [begin, end) in pixels. */
int lineWidth(const char* begin, const char* end, const Font& font, int scale, uint8_t tabSize = 4) {
    int width = 0;
    for (const char* p = begin; p < end; p++) {
        width += advanceOfChar(*p, font, scale, tabSize);
    }
    return width;
}

/** Splits at newlines only. */
LineSlice sourceLine(const char* p) {
    const char* begin = p;
    while (*p != '\0' && *p != '\n') {
        p++;
    }
    const char* end = p;
    const char* next = (*p == '\n') ? p + 1 : p;
    return { begin, end, next };
}

/**
 * Returns the next line that fits in maxWidth, breaking at the last space when there is one.
 * Always makes progress: a single glyph wider than maxWidth is placed on its own line.
 */
LineSlice nextWrappedLine(const char* p, int maxWidth, const Font& font, int scale, uint8_t tabSize) {
    const char* begin = p;
    const char* end = p;
    const char* lastSpace = nullptr;
    int width = 0;

    while (*p != '\0' && *p != '\n') {
        if (*p == '\r') {
            p++;
            continue;
        }
        if (*p == ' ') {
            lastSpace = p;
        }

        const int step = advanceOfChar(*p, font, scale, tabSize);
        if (maxWidth > 0 && width + step > maxWidth && p != begin) {
            if (lastSpace != nullptr && lastSpace > begin) {
                end = lastSpace;
                p = lastSpace + 1;
            } else {
                end = p;
            }
            return { begin, end, p };
        }

        width += step;
        p++;
        end = p;
    }

    if (*p == '\n') {
        p++;
    }
    return { begin, end, p };
}

/**
 * Returns the prefix of [begin, end) that fits in maxWidth, writing its pixel width to outWidth.
 */
const char* ellipsizeLine(const char* begin, const char* end, int maxWidth, const Font& font, int scale, uint8_t tabSize, int* outWidth) {
    const char* result = begin;
    int width = 0;
    for (const char* p = begin; p < end; p++) {
        const int step = advanceOfChar(*p, font, scale, tabSize);
        if (width + step > maxWidth) {
            break;
        }
        width += step;
        result = p + 1;
    }
    *outWidth = width;
    return result;
}

bool glyphBit(const Font& font, const uint8_t* glyph, int row, int col) {
    if (font.layout == FontLayout::Rows) {
        const int bytesPerRow = (font.glyphWidth + 7) / 8;
        return (glyph[row * bytesPerRow + col / 8] & (0x80U >> (col & 7))) != 0;
    }
    if (font.layout == FontLayout::RowsLsb) {
        const int bytesPerRow = (font.glyphWidth + 7) / 8;
        return (glyph[row * bytesPerRow + col / 8] & (1U << (col & 7))) != 0;
    }
    const int bytesPerColumn = (font.glyphHeight + 7) / 8;
    return (glyph[col * bytesPerColumn + row / 8] & (1U << (row & 7))) != 0;
}

int alignedX(int anchor, int width, int boxWidth, TextAlign align) {
    switch (align) {
        case TextAlign::Center:
            return anchor + (boxWidth - width) / 2;
        case TextAlign::Right:
            return anchor + boxWidth - width;
        case TextAlign::Left:
        default:
            return anchor;
    }
}

} // namespace

int measureText(const char* text, const Font& font, uint8_t scale, uint8_t tabSize) {
    if (text == nullptr || *text == '\0') {
        return 0;
    }

    const int magnitude = scale == 0 ? 1 : scale;
    int widest = 0;
    const char* p = text;
    while (true) {
        const LineSlice line = sourceLine(p);
        widest = std::max(widest, lineWidth(line.begin, line.end, font, magnitude, tabSize));
        if (*line.next == '\0') {
            break;
        }
        p = line.next;
    }
    return widest;
}

int measureTextHeight(const char* text, const Font& font, uint8_t scale) {
    if (text == nullptr) {
        return 0;
    }

    const int magnitude = scale == 0 ? 1 : scale;
    int lines = 1;
    for (const char* p = text; *p != '\0'; p++) {
        // A trailing newline doesn't add an empty line that drawing would visit.
        if (*p == '\n' && p[1] != '\0') {
            lines++;
        }
    }
    return lines * lineHeight(font, static_cast<uint8_t>(magnitude));
}

void Surface::drawGlyph(int x, int y, char c, const Font& font, uint16_t foreground, uint16_t background, bool opaque, uint8_t scale) {
    const int magnitude = scale == 0 ? 1 : scale;
    char glyphChar = c;
    const uint8_t* glyph = font.glyph(c);
    if (glyph == nullptr) {
        glyphChar = '?';
        glyph = font.glyph(glyphChar);
    }
    if (glyph == nullptr || font.glyphWidth == 0 || font.glyphHeight == 0) {
        return;
    }

    const int columns = font.widthFor(glyphChar);
    const int advance = font.advanceFor(glyphChar);
    if (opaque) {
        fillRectRaw(
            Rect { x, y, x + advance * magnitude, y + static_cast<int>(font.lineHeight) * magnitude },
            background
        );
    }

    for (int row = 0; row < font.glyphHeight; row++) {
        int runStart = -1;
        for (int col = 0; col <= columns; col++) {
            const bool on = col < columns && glyphBit(font, glyph, row, col);
            if (on) {
                if (runStart < 0) {
                    runStart = col;
                }
            } else if (runStart >= 0) {
                const int x0 = x + runStart * magnitude;
                const int x1 = x + col * magnitude;
                for (int sy = 0; sy < magnitude; sy++) {
                    fillSpanRaw(x0, x1, y + row * magnitude + sy, foreground);
                }
                runStart = -1;
            }
        }
    }
}

void Surface::drawTextLine(int x, int y, const char* begin, const char* end, const TextStyle& style) {
    const Font& font = *style.font;
    const int magnitude = style.scale == 0 ? 1 : style.scale;
    int cursor = x;

    for (const char* p = begin; p < end; p++) {
        const char c = *p;
        if (c == '\r') {
            continue;
        }
        if (c == '\t') {
            cursor += advanceOfChar(c, font, magnitude, style.tabSize);
            continue;
        }
        if (static_cast<uint8_t>(c) < 32) {
            cursor += advanceOfChar(c, font, magnitude, style.tabSize);
            continue;
        }
        drawGlyph(cursor, y, c, font, style.foreground, style.background, style.opaque, static_cast<uint8_t>(magnitude));
        cursor += advanceOfChar(c, font, magnitude, style.tabSize);
    }
}

void Surface::drawText(int x, int y, const char* text, const Font& font, uint16_t foreground) {
    TextStyle style;
    style.font = &font;
    style.foreground = foreground;
    drawText(x, y, text, style);
}

void Surface::drawText(int x, int y, const char* text, const Font& font, uint16_t foreground, uint16_t background) {
    TextStyle style;
    style.font = &font;
    style.foreground = foreground;
    style.background = background;
    style.opaque = true;
    drawText(x, y, text, style);
}

void Surface::drawText(int x, int y, const char* text, const Font& font, uint16_t foreground, uint16_t background, uint8_t scale) {
    TextStyle style;
    style.font = &font;
    style.foreground = foreground;
    style.background = background;
    style.opaque = true;
    style.scale = scale;
    drawText(x, y, text, style);
}

void Surface::drawText(int x, int y, const char* text, const TextStyle& style) {
    if (text == nullptr || *text == '\0') {
        return;
    }

    const TextStyle resolved = normaliseStyle(style);
    const Font& font = *resolved.font;
    const int magnitude = resolved.scale;
    const int lineHeightPixels = static_cast<int>(font.lineHeight) * magnitude;

    int lines = 0;
    int widest = 0;
    const char* p = text;
    while (true) {
        const LineSlice line = sourceLine(p);
        widest = std::max(widest, lineWidth(line.begin, line.end, font, magnitude, resolved.tabSize));
        lines++;
        if (*line.next == '\0') {
            break;
        }
        p = line.next;
    }

    const int blockHeight = lines * lineHeightPixels;
    int startY = y;
    if (resolved.verticalAlign == TextVerticalAlign::Middle) {
        startY = y - blockHeight / 2;
    } else if (resolved.verticalAlign == TextVerticalAlign::Bottom) {
        startY = y - blockHeight;
    }

    int left = x;
    int right = x + widest;
    if (resolved.align == TextAlign::Center) {
        left = x - widest / 2;
        right = left + widest;
    } else if (resolved.align == TextAlign::Right) {
        left = x - widest;
        right = x;
    }
    markDirtyRect(left, startY, right, startY + blockHeight);

    p = text;
    int lineY = startY;
    while (true) {
        const LineSlice line = sourceLine(p);
        const int width = lineWidth(line.begin, line.end, font, magnitude, resolved.tabSize);
        int lineX = x;
        if (resolved.align == TextAlign::Center) {
            lineX = x - width / 2;
        } else if (resolved.align == TextAlign::Right) {
            lineX = x - width;
        }
        drawTextLine(lineX, lineY, line.begin, line.end, resolved);

        if (*line.next == '\0') {
            break;
        }
        p = line.next;
        lineY += lineHeightPixels;
    }
}

void Surface::drawText(const Rect& box, const char* text, const TextStyle& style) {
    if (text == nullptr || *text == '\0' || box.empty()) {
        return;
    }

    const TextStyle resolved = normaliseStyle(style);
    const Font& font = *resolved.font;
    const int magnitude = resolved.scale;
    const int lineHeightPixels = static_cast<int>(font.lineHeight) * magnitude;

    int lines = 0;
    int widest = 0;
    if (resolved.wrap == TextWrap::Word) {
        const char* p = text;
        while (true) {
            const LineSlice line = nextWrappedLine(p, box.width(), font, magnitude, resolved.tabSize);
            widest = std::max(widest, lineWidth(line.begin, line.end, font, magnitude, resolved.tabSize));
            lines++;
            if (*line.next == '\0') {
                break;
            }
            p = line.next;
        }
    } else {
        const char* p = text;
        while (true) {
            const LineSlice line = sourceLine(p);
            widest = std::max(widest, lineWidth(line.begin, line.end, font, magnitude, resolved.tabSize));
            lines++;
            if (*line.next == '\0') {
                break;
            }
            p = line.next;
        }
    }

    const int blockHeight = lines * lineHeightPixels;
    int startY = box.y0;
    if (resolved.verticalAlign == TextVerticalAlign::Middle) {
        startY = box.y0 + (box.height() - blockHeight) / 2;
    } else if (resolved.verticalAlign == TextVerticalAlign::Bottom) {
        startY = box.y0 + box.height() - blockHeight;
    }

    const int left = alignedX(box.x0, widest, box.width(), resolved.align);
    markDirtyRect(
        std::max(left, box.x0),
        std::max(startY, box.y0),
        std::min(left + widest, box.x1),
        std::min(startY + blockHeight, box.y1)
    );

    // Clip to the box, so text that is too wide or too tall cannot leak into neighbouring content.
    const Rect previousClip = clip_;
    setClip(box);

    int lineY = startY;
    const char* const ellipsisText = "...";
    const int ellipsisWidth = resolved.ellipsis
        ? measureText(ellipsisText, font, static_cast<uint8_t>(magnitude), resolved.tabSize)
        : 0;

    if (resolved.wrap == TextWrap::Word) {
        const char* p = text;
        while (true) {
            const LineSlice line = nextWrappedLine(p, box.width(), font, magnitude, resolved.tabSize);
            const int width = lineWidth(line.begin, line.end, font, magnitude, resolved.tabSize);
            const int lineX = alignedX(box.x0, width, box.width(), resolved.align);
            drawTextLine(lineX, lineY, line.begin, line.end, resolved);
            if (*line.next == '\0') {
                break;
            }
            p = line.next;
            lineY += lineHeightPixels;
        }
    } else {
        const char* p = text;
        while (true) {
            const LineSlice line = sourceLine(p);
            const int fullWidth = lineWidth(line.begin, line.end, font, magnitude, resolved.tabSize);
            const char* drawEnd = line.end;
            int renderedWidth = fullWidth;
            int prefixWidth = fullWidth;

            if (resolved.ellipsis && fullWidth > box.width()) {
                const int budget = box.width() - ellipsisWidth;
                drawEnd = ellipsizeLine(line.begin, line.end, budget, font, magnitude, resolved.tabSize, &prefixWidth);
                renderedWidth = prefixWidth + ellipsisWidth;
            }

            const int lineX = alignedX(box.x0, renderedWidth, box.width(), resolved.align);
            drawTextLine(lineX, lineY, line.begin, drawEnd, resolved);
            if (drawEnd != line.end) {
                drawTextLine(lineX + prefixWidth, lineY, ellipsisText, ellipsisText + 3, resolved);
            }

            if (*line.next == '\0') {
                break;
            }
            p = line.next;
            lineY += lineHeightPixels;
        }
    }

    clip_ = previousClip;
}

} // namespace gfx
