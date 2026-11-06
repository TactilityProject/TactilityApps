// SPDX-License-Identifier: GPL-3.0-or-later
#include "Canvas.h"

#include "../render/Raster.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace maps {

void Canvas::blendPixel(int x, int y, uint16_t color, uint32_t alpha32) {
    if (x < 0 || y < 0 || x >= width || y >= height || alpha32 == 0) {
        return;
    }
    uint16_t& pixel = pixels[y * width + x];
    pixel = alpha32 >= 32 ? color : blendRgb565(color, pixel, alpha32);
}

void Canvas::fillRect(int x, int y, int w, int h, uint16_t color, uint8_t opacity) {
    const uint32_t alpha = (opacity + 4u) >> 3;
    const int top = std::max(0, y);
    const int bottom = std::min(height, y + h);
    const int left = std::max(0, x);
    const int right = std::min(width, x + w);
    for (int row = top; row < bottom; row++) {
        uint16_t* line = pixels + row * width;
        for (int column = left; column < right; column++) {
            line[column] = alpha >= 32 ? color : blendRgb565(color, line[column], alpha);
        }
    }
}

void Canvas::frameRect(int x, int y, int w, int h, uint16_t color, int thickness) {
    fillRect(x, y, w, thickness, color);
    fillRect(x, y + h - thickness, w, thickness, color);
    fillRect(x, y + thickness, thickness, h - 2 * thickness, color);
    fillRect(x + w - thickness, y + thickness, thickness, h - 2 * thickness, color);
}

void Canvas::fillRoundRect(int x, int y, int w, int h, int radius, uint16_t color, uint8_t opacity) {
    radius = std::min(radius, std::min(w, h) / 2);
    if (radius <= 0) {
        fillRect(x, y, w, h, color, opacity);
        return;
    }
    const float scale = static_cast<float>(opacity) * (32.0f / 255.0f);
    const int top = std::max(0, y);
    const int bottom = std::min(height, y + h);
    for (int row = top; row < bottom; row++) {
        // Distance into a corner row, measured from the corner circle's centre
        const float py = static_cast<float>(row) + 0.5f;
        float inset = 0.0f;
        float dy = 0.0f;
        if (py < y + radius) {
            dy = static_cast<float>(y + radius) - py;
        } else if (py > y + h - radius) {
            dy = py - static_cast<float>(y + h - radius);
        }
        if (dy > 0.0f) {
            inset = static_cast<float>(radius) - std::sqrt(std::max(0.0f, static_cast<float>(radius * radius) - dy * dy));
        }
        const float leftEdge = static_cast<float>(x) + inset;
        const float rightEdge = static_cast<float>(x + w) - inset;
        const int first = std::max(0, static_cast<int>(std::floor(leftEdge)));
        const int last = std::min(width - 1, static_cast<int>(std::ceil(rightEdge)) - 1);
        for (int column = first; column <= last; column++) {
            const float cellLeft = static_cast<float>(column);
            const float covered = std::min(cellLeft + 1.0f, rightEdge) - std::max(cellLeft, leftEdge);
            if (covered > 0.0f) {
                blendPixel(column, row, color, static_cast<uint32_t>(std::min(1.0f, covered) * scale + 0.5f));
            }
        }
    }
}

void Canvas::fillCircle(float cx, float cy, float radius, uint16_t color, uint8_t opacity) {
    const float scale = static_cast<float>(opacity) * (32.0f / 255.0f);
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - 1.0f)));
    const int bottom = std::min(height - 1, static_cast<int>(std::ceil(cy + radius + 1.0f)));
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - 1.0f)));
    const int right = std::min(width - 1, static_cast<int>(std::ceil(cx + radius + 1.0f)));
    for (int row = top; row <= bottom; row++) {
        for (int column = left; column <= right; column++) {
            const float dx = static_cast<float>(column) + 0.5f - cx;
            const float dy = static_cast<float>(row) + 0.5f - cy;
            const float coverage = std::clamp(radius + 0.5f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
            blendPixel(column, row, color, static_cast<uint32_t>(coverage * scale + 0.5f));
        }
    }
}

void Canvas::ring(float cx, float cy, float radius, float thickness, uint16_t color, uint8_t opacity) {
    const float scale = static_cast<float>(opacity) * (32.0f / 255.0f);
    const float half = thickness * 0.5f;
    const int top = std::max(0, static_cast<int>(std::floor(cy - radius - half - 1.0f)));
    const int bottom = std::min(height - 1, static_cast<int>(std::ceil(cy + radius + half + 1.0f)));
    const int left = std::max(0, static_cast<int>(std::floor(cx - radius - half - 1.0f)));
    const int right = std::min(width - 1, static_cast<int>(std::ceil(cx + radius + half + 1.0f)));
    for (int row = top; row <= bottom; row++) {
        for (int column = left; column <= right; column++) {
            const float dx = static_cast<float>(column) + 0.5f - cx;
            const float dy = static_cast<float>(row) + 0.5f - cy;
            const float distance = std::fabs(std::sqrt(dx * dx + dy * dy) - radius);
            const float coverage = std::clamp(half + 0.5f - distance, 0.0f, 1.0f);
            blendPixel(column, row, color, static_cast<uint32_t>(coverage * scale + 0.5f));
        }
    }
}

void Canvas::line(float x0, float y0, float x1, float y1, float lineWidth, uint16_t color, uint8_t opacity) {
    const float scale = static_cast<float>(opacity) * (32.0f / 255.0f);
    const float half = lineWidth * 0.5f;
    const int top = std::max(0, static_cast<int>(std::floor(std::min(y0, y1) - half - 1.0f)));
    const int bottom = std::min(height - 1, static_cast<int>(std::ceil(std::max(y0, y1) + half + 1.0f)));
    const int left = std::max(0, static_cast<int>(std::floor(std::min(x0, x1) - half - 1.0f)));
    const int right = std::min(width - 1, static_cast<int>(std::ceil(std::max(x0, x1) + half + 1.0f)));
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float lengthSquared = dx * dx + dy * dy;
    for (int row = top; row <= bottom; row++) {
        for (int column = left; column <= right; column++) {
            const float px = static_cast<float>(column) + 0.5f - x0;
            const float py = static_cast<float>(row) + 0.5f - y0;
            const float t = lengthSquared > 0.0f ? std::clamp((px * dx + py * dy) / lengthSquared, 0.0f, 1.0f) : 0.0f;
            const float ex = px - t * dx;
            const float ey = py - t * dy;
            const float coverage = std::clamp(half + 0.5f - std::sqrt(ex * ex + ey * ey), 0.0f, 1.0f);
            blendPixel(column, row, color, static_cast<uint32_t>(coverage * scale + 0.5f));
        }
    }
}

UiTheme uiTheme(const Palette& palette, bool monochrome) {
    UiTheme theme {};
    theme.panel = palette.halo;
    theme.text = palette.text;
    theme.accent = palette.marker;
    theme.accentText = palette.halo;
    theme.border = palette.minorCasing;
    if (monochrome) {
        theme.muted = palette.text;
        theme.danger = palette.text;
        theme.dangerText = palette.halo;
        theme.scrim = palette.halo;
        theme.panelOpacity = 255;
        theme.controlOpacity = 255;
    } else {
        theme.muted = palette.rail;
        theme.danger = palette.motorwayCasing;
        theme.dangerText = palette.halo;
        theme.scrim = palette.text;
        theme.panelOpacity = 240;
        theme.controlOpacity = 200;
    }
    return theme;
}

int wrapText(Font& font, const char* text, int maxWidth, char* lines, size_t lineSize, int maxLines) {
    int count = 0;
    const char* cursor = text;
    while (*cursor != '\0' && count < maxLines) {
        while (*cursor == ' ') {
            cursor++;
        }
        char* line = lines + static_cast<size_t>(count) * lineSize;
        size_t length = 0;
        size_t lastBreak = 0;
        const char* lastBreakSource = nullptr;
        line[0] = '\0';
        while (cursor[length] != '\0' && length + 1 < lineSize) {
            line[length] = cursor[length];
            line[length + 1] = '\0';
            if (font.textWidth(line) > maxWidth && length > 0) {
                line[length] = '\0';
                break;
            }
            if (cursor[length] == ' ') {
                lastBreak = length;
                lastBreakSource = cursor + length;
            }
            length++;
        }
        const bool ended = cursor[length] == '\0';
        if (!ended && lastBreakSource != nullptr && count + 1 < maxLines) {
            // Break at the last space that fits
            line[lastBreak] = '\0';
            cursor = lastBreakSource + 1;
        } else {
            line[length] = '\0';
            cursor += length;
        }
        count++;
    }
    return count;
}

void drawClippedText(Canvas& canvas, Font& font, int x, int y, const char* text, int maxWidth, uint16_t color) {
    if (font.textWidth(text) <= maxWidth) {
        font.drawText(canvas, x, y, text, color);
        return;
    }
    char clipped[96];
    size_t length = strlen(text);
    if (length >= sizeof(clipped) - 3) {
        length = sizeof(clipped) - 3;
    }
    while (length > 0) {
        memcpy(clipped, text, length);
        // Never cut inside a UTF-8 character
        while (length > 0 && (static_cast<uint8_t>(clipped[length - 1]) & 0xC0) == 0x80) {
            length--;
        }
        if (length > 0 && (static_cast<uint8_t>(clipped[length - 1]) & 0xC0) == 0xC0) {
            length--;
        }
        clipped[length] = '.';
        clipped[length + 1] = '.';
        clipped[length + 2] = '\0';
        if (font.textWidth(clipped) <= maxWidth) {
            break;
        }
        length--;
    }
    font.drawText(canvas, x, y, clipped, color);
}

} // namespace maps
