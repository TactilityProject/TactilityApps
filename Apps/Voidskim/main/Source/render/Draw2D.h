#pragma once

#include <cstdint>

namespace hd {

/** An RGB565 image to draw 2D overlays (HUD, menus, touch controls) on. */
struct Canvas {
    uint16_t* pixels = nullptr;
    int width = 0;
    int height = 0;
    /**
     * Optional, width x height: every pixel drawn by these functions is marked 1. On monochrome
     * displays marked (UI) pixels are shown as plain black or white, and the rest (the 3D world)
     * dithered.
     */
    uint8_t* uiMask = nullptr;
};

/** Converts 0xRRGGBB to RGB565. */
constexpr uint16_t rgb565(uint32_t rgb) {
    return (uint16_t)(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
}

/** Scales an RGB565 colour's brightness by factor/256. */
uint16_t shade565(uint16_t colour, int factor);

void fillRect(Canvas& canvas, int x, int y, int w, int h, uint16_t colour);
/** Blends a colour over a rectangle, alpha 0..256. */
void blendRect(Canvas& canvas, int x, int y, int w, int h, uint16_t colour, int alpha);
void drawRect(Canvas& canvas, int x, int y, int w, int h, uint16_t colour);
void drawLine(Canvas& canvas, int x0, int y0, int x1, int y1, uint16_t colour);
void fillCircle(Canvas& canvas, int cx, int cy, int r, uint16_t colour);
void blendCircle(Canvas& canvas, int cx, int cy, int r, uint16_t colour, int alpha);
void drawCircle(Canvas& canvas, int cx, int cy, int r, uint16_t colour);
/** A filled triangle pointing in a direction: 0 up, 1 right, 2 down, 3 left. */
void fillArrow(Canvas& canvas, int cx, int cy, int size, int direction, uint16_t colour);

/** Width of a text in the 5x7 font at an integer scale. */
int textWidth(const char* text, int scale);
constexpr int textHeight(int scale) { return 7 * scale; }
/** Draws text in the 5x7 font, with a dark drop shadow for legibility. Lowercase is shown as uppercase. */
void drawText(Canvas& canvas, int x, int y, const char* text, int scale, uint16_t colour, bool shadow = true);
void drawTextCentered(Canvas& canvas, int centreX, int y, const char* text, int scale, uint16_t colour, bool shadow = true);
void drawTextRight(Canvas& canvas, int rightX, int y, const char* text, int scale, uint16_t colour, bool shadow = true);

} // namespace hd
