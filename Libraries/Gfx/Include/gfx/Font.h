// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace gfx {

/** How a font's 1bpp glyph rows are packed. */
enum class FontLayout {
    /** One byte per row, most significant bit is the leftmost pixel. */
    Rows,
    /** One byte per row, least significant bit is the leftmost pixel (the public-domain 8x8 font). */
    RowsLsb,
    /** One byte per column, least significant bit is the top pixel (the classic 5x7 layout). */
    Columns,
};

/**
 * A bitmap font.
 *
 * Glyphs are stored contiguously for characters [firstChar, lastChar]. A glyph occupies
 * ceil(glyphWidth / 8) * glyphHeight bytes, in the order described by `layout`. Fonts are constant
 * data and are never allocated.
 *
 * Native fonts are used for small text; `TextStyle::scale` magnifies them for larger text. Scaling a
 * small font is cheap and crisp enough at integer scales, but a native font at the target size looks
 * better for body text.
 */
struct Font {
    uint8_t glyphWidth = 0;
    uint8_t glyphHeight = 0;
    /** Horizontal advance at scale 1, including the gap to the next glyph. */
    uint8_t advance = 0;
    /** Vertical advance at scale 1, including the line gap. */
    uint8_t lineHeight = 0;
    uint8_t firstChar = 0;
    uint8_t lastChar = 0;
    FontLayout layout = FontLayout::Rows;
    const uint8_t* data = nullptr;
    /**
     * Optional per-glyph visual width for [firstChar, lastChar]. Null means glyphWidth.
     * Values must not exceed glyphWidth; columns beyond the value are ignored.
     */
    const uint8_t* glyphWidths = nullptr;
    /**
     * Optional per-glyph advance for [firstChar, lastChar]. Null means advance.
     * This is what makes a fixed-cell font proportional.
     */
    const uint8_t* advances = nullptr;

    bool contains(char c) const {
        const auto value = static_cast<uint8_t>(c);
        return data != nullptr && value >= firstChar && value <= lastChar;
    }

    const uint8_t* glyph(char c) const { return contains(c) ? data + (static_cast<uint8_t>(c) - firstChar) * bytesPerGlyph() : nullptr; }

    /** Visible columns for this glyph (at most glyphWidth). */
    uint8_t widthFor(char c) const {
        const auto value = static_cast<uint8_t>(c);
        if (glyphWidths == nullptr || value < firstChar || value > lastChar) {
            return glyphWidth;
        }
        const uint8_t width = glyphWidths[value - firstChar];
        return width > glyphWidth ? glyphWidth : width;
    }

    /** Horizontal step for this glyph at scale 1. */
    uint8_t advanceFor(char c) const {
        const auto value = static_cast<uint8_t>(c);
        if (advances == nullptr || value < firstChar || value > lastChar) {
            return advance;
        }
        return advances[value - firstChar];
    }

    int bytesPerGlyph() const {
        if (layout == FontLayout::Rows || layout == FontLayout::RowsLsb) {
            return ((glyphWidth + 7) / 8) * glyphHeight;
        }
        return glyphWidth * ((glyphHeight + 7) / 8);
    }
};

/** 5x7 glyphs, ASCII 32-126, classic column-major layout. */
extern const Font FONT_5X7;

/** 5x7 glyphs with per-glyph widths and advances (proportional variant). */
extern const Font FONT_5X7_PROPORTIONAL;

/** 8x8 glyphs, ASCII 0-127, row-major layout. Public-domain VGA-style font. */
extern const Font FONT_8X8;

/** 8x16 Terminus glyphs, Latin-1 32-255. OFL-1.1; see Licenses/Terminus-OFL.txt. */
extern const Font FONT_8X16;

enum class TextAlign {
    Left,
    Center,
    Right,
};

enum class TextVerticalAlign {
    Top,
    Middle,
    Bottom,
};

enum class TextWrap {
    None,
    /** Wrap at spaces where possible, otherwise between characters. */
    Word,
};

/** Everything needed to draw a run of text. */
struct TextStyle {
    const Font* font = &FONT_5X7;
    /** Glyph colour, RGB565. */
    uint16_t foreground = 0xFFFF;
    /** Cell background, RGB565. Only used when `opaque` is true. */
    uint16_t background = 0x0000;
    /** When false, only glyph pixels are written and the background is left untouched. */
    bool opaque = false;
    /** Integer magnification. 1 draws the font at its native size. */
    uint8_t scale = 1;
    /** Number of cells a tab advances in drawText()/measureText(). */
    uint8_t tabSize = 4;
    /** When true, the boxed drawText() truncates overlong lines with "...". */
    bool ellipsis = false;
    TextAlign align = TextAlign::Left;
    TextVerticalAlign verticalAlign = TextVerticalAlign::Top;
    TextWrap wrap = TextWrap::None;
};

} // namespace gfx
