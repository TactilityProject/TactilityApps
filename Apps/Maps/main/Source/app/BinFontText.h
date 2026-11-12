// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../render/LabelLayout.h"
#include "../ui/Canvas.h"

#include <cstdint>

struct BinFont;
struct PixelBuffer;

namespace maps {

/**
 * Text in one of Tactility's cached fonts (<data>/cache/fonts), drawn with the binfont-module. As a LabelFont it gives
 * glyph masks for map labels.
 */
class BinFontText final : public Font, public LabelFont {
public:
    ~BinFontText() override { close(); }

    /**
     * Opens the first font whose file name starts with one of the prefixes. Call it on the I/O task: the binfont-module
     * loads the whole file into PSRAM, so drawing never reads the card.
     */
    bool open(const char* folder, const char* const* prefixes, int prefixCount);
    /** Opens one font file (read whole into PSRAM, so on the I/O task). */
    bool openFile(const char* path);
    /**
     * Uses the text font Tactility's UI uses (an LvglFontSize), without a copy. Firmware builds with the fonts inside
     * have no font files on the card, so this comes first. Fails when that font is not a BinFont.
     */
    bool borrow(int lvglFontSize);
    void close();
    bool isOpen() const { return font != nullptr; }

    int lineHeight() const override { return font != nullptr ? height : 10; }
    int ascent() const override { return ascentPixels; }
    bool glyph(uint32_t codepoint, LabelGlyph& out, bool withMask) override;
    int kerningX16(uint32_t leftId, uint32_t rightId) override;
    int textWidth(const char* text) override;
    void drawText(Canvas& canvas, int x, int y, const char* text, uint16_t color) override;

private:
    BinFont* font = nullptr;
    /** Borrowed fonts belong to Tactility and are not closed. */
    bool owned = false;
    int height = 0;
    int ascentPixels = 0;
    /** The mask of the last glyph asked for with withMask; labels use fonts of at most about 30 px. */
    static constexpr int MASK_BYTES = 48 * 48;
    uint8_t mask[MASK_BYTES] {};
    /** The canvas the text was last drawn on, wrapped once as a PixelBuffer. */
    PixelBuffer* target = nullptr;
    uint16_t* targetPixels = nullptr;
};

} // namespace maps
