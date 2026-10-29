// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../render/Style.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** An RGB565 picture the UI draws into, the frame that goes to the display. */
struct Canvas {
    uint16_t* pixels = nullptr;
    int width = 0;
    int height = 0;

    void fillRect(int x, int y, int w, int h, uint16_t color, uint8_t opacity = 255);
    void frameRect(int x, int y, int w, int h, uint16_t color, int thickness = 1);
    /** A rectangle with corners rounded by radius pixels, anti-aliased. */
    void fillRoundRect(int x, int y, int w, int h, int radius, uint16_t color, uint8_t opacity = 255);
    void fillCircle(float cx, float cy, float radius, uint16_t color, uint8_t opacity = 255);
    /** A circle outline of the given thickness, anti-aliased. */
    void ring(float cx, float cy, float radius, float thickness, uint16_t color, uint8_t opacity = 255);
    /** A straight line with round ends, anti-aliased. */
    void line(float x0, float y0, float x1, float y1, float width, uint16_t color, uint8_t opacity = 255);
    void blendPixel(int x, int y, uint16_t color, uint32_t alpha32);
};

/** Text drawing, from Tactility's fonts on the device and a stand-in on the PC. */
class Font {
public:
    virtual ~Font() = default;
    virtual int lineHeight() const = 0;
    virtual int textWidth(const char* text) = 0;
    /** Draws UTF-8 text with the top of its line at y. */
    virtual void drawText(Canvas& canvas, int x, int y, const char* text, uint16_t color) = 0;
};

/** The colours of the app's own UI, from the map palette, so monochrome displays get a high contrast UI. */
struct UiTheme {
    uint16_t panel;
    uint16_t text;
    uint16_t muted;
    uint16_t accent;
    uint16_t accentText;
    uint16_t danger;
    uint16_t dangerText;
    uint16_t scrim;
    uint16_t border;
    /** Panels over the map are slightly see-through on colour displays, solid on monochrome ones. */
    uint8_t panelOpacity;
    uint8_t controlOpacity;
};

UiTheme uiTheme(const Palette& palette, bool monochrome);

/**
 * Splits text into lines that fit maxWidth, breaking at spaces. Writes up to maxLines lines of up to
 * lineSize - 1 characters. Returns the number of lines.
 */
int wrapText(Font& font, const char* text, int maxWidth, char* lines, size_t lineSize, int maxLines);

/** Draws text cut to maxWidth, ending in ".." when it does not fit. */
void drawClippedText(Canvas& canvas, Font& font, int x, int y, const char* text, int maxWidth, uint16_t color);

} // namespace maps
