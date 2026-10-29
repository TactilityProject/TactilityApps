// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// The PC stand-in for Tactility's fonts: every character is a small block, so layouts can be checked and
// previewed. Sized like the T-Deck's default font: 16 px lines, 8 px per character.

#include "../main/Source/ui/Canvas.h"

#include <cstring>

namespace maps {

class FakeFont final : public Font {
public:
    explicit FakeFont(int advance = 8, int line = 16) : advance(advance), line(line) {}

    int lineHeight() const override { return line; }

    int textWidth(const char* text) override { return characters(text) * advance; }

    void drawText(Canvas& canvas, int x, int y, const char* text, uint16_t color) override {
        for (const char* cursor = text; *cursor != '\0'; cursor++) {
            if ((static_cast<unsigned char>(*cursor) & 0xC0) == 0x80) {
                continue;
            }
            if (*cursor != ' ') {
                // A block of the x-height, taller for capitals and digits
                const bool tall = (*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= '0' && *cursor <= '9');
                const int top = tall ? 3 : 6;
                canvas.fillRect(x + 1, y + top, advance - 2, line - top - 4, color);
            }
            x += advance;
        }
    }

private:
    static int characters(const char* text) {
        int count = 0;
        for (; *text != '\0'; text++) {
            if ((static_cast<unsigned char>(*text) & 0xC0) != 0x80) {
                count++;
            }
        }
        return count;
    }

    int advance;
    int line;
};

} // namespace maps
