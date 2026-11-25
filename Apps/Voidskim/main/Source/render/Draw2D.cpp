#include "Draw2D.h"

#include <cstdlib>

namespace hd {

#include "Font5x7.inc"

uint16_t shade565(uint16_t colour, int factor) {
    int r = ((colour >> 11) & 31) * factor >> 8;
    int g = ((colour >> 5) & 63) * factor >> 8;
    int b = (colour & 31) * factor >> 8;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static inline uint16_t blend565(uint16_t dst, uint16_t src, int alpha) {
    const int inv = 256 - alpha;
    const int r = (((dst >> 11) & 31) * inv + ((src >> 11) & 31) * alpha) >> 8;
    const int g = (((dst >> 5) & 63) * inv + ((src >> 5) & 63) * alpha) >> 8;
    const int b = ((dst & 31) * inv + (src & 31) * alpha) >> 8;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static bool clip(const Canvas& canvas, int& x, int& y, int& w, int& h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > canvas.width) w = canvas.width - x;
    if (y + h > canvas.height) h = canvas.height - y;
    return w > 0 && h > 0;
}

static inline void markUi(Canvas& canvas, int x, int y, int w, int h) {
    if (canvas.uiMask == nullptr) return;
    for (int row = y; row < y + h; row++) {
        uint8_t* m = canvas.uiMask + row * canvas.width + x;
        for (int i = 0; i < w; i++) m[i] = 1;
    }
}

void fillRect(Canvas& canvas, int x, int y, int w, int h, uint16_t colour) {
    if (!clip(canvas, x, y, w, h)) return;
    markUi(canvas, x, y, w, h);
    for (int row = y; row < y + h; row++) {
        uint16_t* p = canvas.pixels + row * canvas.width + x;
        for (int i = 0; i < w; i++) p[i] = colour;
    }
}

void blendRect(Canvas& canvas, int x, int y, int w, int h, uint16_t colour, int alpha) {
    if (!clip(canvas, x, y, w, h)) return;
    markUi(canvas, x, y, w, h);
    for (int row = y; row < y + h; row++) {
        uint16_t* p = canvas.pixels + row * canvas.width + x;
        for (int i = 0; i < w; i++) p[i] = blend565(p[i], colour, alpha);
    }
}

void drawRect(Canvas& canvas, int x, int y, int w, int h, uint16_t colour) {
    fillRect(canvas, x, y, w, 1, colour);
    fillRect(canvas, x, y + h - 1, w, 1, colour);
    fillRect(canvas, x, y, 1, h, colour);
    fillRect(canvas, x + w - 1, y, 1, h, colour);
}

static inline void plot(Canvas& canvas, int x, int y, uint16_t colour) {
    if (x >= 0 && y >= 0 && x < canvas.width && y < canvas.height) {
        canvas.pixels[y * canvas.width + x] = colour;
        if (canvas.uiMask != nullptr) canvas.uiMask[y * canvas.width + x] = 1;
    }
}

void drawLine(Canvas& canvas, int x0, int y0, int x1, int y1, uint16_t colour) {
    const int dx = abs(x1 - x0);
    const int dy = -abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        plot(canvas, x0, y0, colour);
        if (x0 == x1 && y0 == y1) break;
        const int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fillCircle(Canvas& canvas, int cx, int cy, int r, uint16_t colour) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        fillRect(canvas, cx - dx, cy + dy, dx * 2 + 1, 1, colour);
    }
}

void blendCircle(Canvas& canvas, int cx, int cy, int r, uint16_t colour, int alpha) {
    for (int dy = -r; dy <= r; dy++) {
        int dx = 0;
        while ((dx + 1) * (dx + 1) + dy * dy <= r * r) dx++;
        blendRect(canvas, cx - dx, cy + dy, dx * 2 + 1, 1, colour, alpha);
    }
}

void drawCircle(Canvas& canvas, int cx, int cy, int r, uint16_t colour) {
    // Midpoint circle
    int x = r;
    int y = 0;
    int err = 1 - r;
    while (x >= y) {
        plot(canvas, cx + x, cy + y, colour); plot(canvas, cx + y, cy + x, colour);
        plot(canvas, cx - y, cy + x, colour); plot(canvas, cx - x, cy + y, colour);
        plot(canvas, cx - x, cy - y, colour); plot(canvas, cx - y, cy - x, colour);
        plot(canvas, cx + y, cy - x, colour); plot(canvas, cx + x, cy - y, colour);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void fillArrow(Canvas& canvas, int cx, int cy, int size, int direction, uint16_t colour) {
    // Rows of a triangle growing from the tip
    for (int i = 0; i <= size; i++) {
        const int half = i / 2 + (i > 0 ? 1 : 0) - 1;
        const int along = i - size / 2;
        switch (direction & 3) {
            case 0: fillRect(canvas, cx - half, cy + along, half * 2 + 1, 1, colour); break;
            case 2: fillRect(canvas, cx - half, cy - along, half * 2 + 1, 1, colour); break;
            case 1: fillRect(canvas, cx - along, cy - half, 1, half * 2 + 1, colour); break;
            case 3: fillRect(canvas, cx + along, cy - half, 1, half * 2 + 1, colour); break;
        }
    }
}

static const uint8_t* glyph(char c) {
    if (c >= 'a' && c <= 'z') c = (char)(c - 32);
    if (c < 32 || c > 95) c = '?';
    return FONT_5X7[c - 32];
}

int textWidth(const char* text, int scale) {
    int n = 0;
    for (const char* p = text; *p != '\0'; p++) n++;
    return n > 0 ? (n * 6 - 1) * scale : 0;
}

static void drawGlyphs(Canvas& canvas, int x, int y, const char* text, int scale, uint16_t colour) {
    for (const char* p = text; *p != '\0'; p++, x += 6 * scale) {
        const uint8_t* rows = glyph(*p);
        for (int row = 0; row < 7; row++) {
            for (int col = 0; col < 5; col++) {
                if (rows[row] & (0x10 >> col)) {
                    fillRect(canvas, x + col * scale, y + row * scale, scale, scale, colour);
                }
            }
        }
    }
}

void drawText(Canvas& canvas, int x, int y, const char* text, int scale, uint16_t colour, bool shadow) {
    if (shadow && canvas.uiMask != nullptr) {
        // Black and white display: a full outline, as a drop shadow alone gets lost in the dither
        for (int oy = -1; oy <= 1; oy++) {
            for (int ox = -1; ox <= 1; ox++) {
                if (ox != 0 || oy != 0) drawGlyphs(canvas, x + ox * scale, y + oy * scale, text, scale, 0x0000);
            }
        }
    } else if (shadow) {
        drawGlyphs(canvas, x + scale, y + scale, text, scale, 0x0000);
    }
    drawGlyphs(canvas, x, y, text, scale, colour);
}

void drawTextCentered(Canvas& canvas, int centreX, int y, const char* text, int scale, uint16_t colour, bool shadow) {
    drawText(canvas, centreX - textWidth(text, scale) / 2, y, text, scale, colour, shadow);
}

void drawTextRight(Canvas& canvas, int rightX, int y, const char* text, int scale, uint16_t colour, bool shadow) {
    drawText(canvas, rightX - textWidth(text, scale), y, text, scale, colour, shadow);
}

} // namespace hd
