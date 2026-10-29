// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../ui/Canvas.h"

#include <cstdint>

struct BinFont;

namespace maps {

/** Text in one of Tactility's cached fonts (<data>/cache/fonts), glyphs loaded once on first use. */
class BinFontText final : public Font {
public:
    ~BinFontText() override { close(); }

    /** Opens the first font whose file name starts with one of the prefixes. Call it on the I/O task. */
    bool open(const char* folder, const char* const* prefixes, int prefixCount);
    void close();
    bool isOpen() const { return font != nullptr; }
    /** Loads the glyphs of printable ASCII and ©, so drawing never reads the card. */
    void preload();

    int lineHeight() const override { return font != nullptr ? height : 10; }
    int textWidth(const char* text) override;
    void drawText(Canvas& canvas, int x, int y, const char* text, uint16_t color) override;

private:
    struct Glyph {
        int16_t x;
        int16_t y;
        uint16_t width;
        uint16_t height;
        uint16_t advance;
        bool loaded;
        uint8_t* alpha;
    };

    Glyph* glyph(uint32_t codepoint);

    BinFont* font = nullptr;
    int height = 0;
    int ascent = 0;
    Glyph glyphs[96] {};
    Glyph copyright {};
};

} // namespace maps
