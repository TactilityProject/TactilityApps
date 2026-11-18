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
    /**
     * The text to draw: letters the font has no glyph for (Tactility's UI font covers ASCII and Latin-1, so "Spisska"
     * would lose its s with caron) become their base letter, keeping upper case. The text itself when nothing is missing.
     */
    const char* withFallback(const char* text);
    char fallbackText[256] {};
    BinFont* font = nullptr;
    /** Borrowed fonts belong to Tactility and are not closed. */
    bool owned = false;
    int height = 0;
    int ascentPixels = 0;
    /** The mask of the last glyph asked for with withMask; labels use fonts of at most about 30 px. */
    static constexpr int MASK_BYTES = 48 * 48;
    uint8_t mask[MASK_BYTES] {};
    /**
     * The glyphs asked for before, with their masks: looking a glyph up and unpacking its bitmap in the binfont-module
     * took about 2 ms per label, most of the time of placing the labels of a view (15 Nov). Direct mapped by
     * codepoint; the masks in one block of PSRAM that is started again when it is full.
     */
    struct CachedGlyph {
        uint32_t codepoint;
        uint32_t id;
        int32_t advanceX16;
        /** Into glyphMasks; NO_MASK when the mask was not asked for yet (or is too large). */
        uint32_t maskOffset;
        int16_t left;
        int16_t top;
        uint16_t width;
        uint16_t height;
        bool used;
    };
    static constexpr int GLYPH_SLOTS = 256;
    static constexpr uint32_t GLYPH_MASK_BYTES = 24 * 1024;
    static constexpr uint32_t NO_MASK = UINT32_MAX;
    void dropGlyphs();
    CachedGlyph* glyphs = nullptr;
    uint8_t* glyphMasks = nullptr;
    uint32_t glyphMasksUsed = 0;
    /** The canvas the text was last drawn on, wrapped once as a PixelBuffer. */
    PixelBuffer* target = nullptr;
    uint16_t* targetPixels = nullptr;
};

} // namespace maps
