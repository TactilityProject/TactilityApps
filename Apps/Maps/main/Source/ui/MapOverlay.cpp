// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapOverlay.h"

#include "../render/Raster.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace maps {

namespace {

constexpr const char* ATTRIBUTION = "\xC2\xA9 OpenMapTiles \xC2\xA9 OpenStreetMap contributors";
constexpr const char* ATTRIBUTION_FIRST = "\xC2\xA9 OpenMapTiles";
constexpr const char* ATTRIBUTION_SECOND = "\xC2\xA9 OpenStreetMap contributors";
constexpr const char* ATTRIBUTION_MARK = "\xC2\xA9";

} // namespace

const char* MapOverlay::attributionText() {
    return ATTRIBUTION;
}

void MapOverlay::drawAttribution(Canvas& canvas, const Palette& palette, int& boxX, int& boxY, int& boxWidth, int& boxHeight) {
    // One line when it fits, otherwise the two credits on two lines, never a font under 12 px
    const int height = normal->lineHeight();
    const bool fits = normal->textWidth(ATTRIBUTION) + 2 * MARGIN <= canvas.width;
    const char* const lines[] = { ATTRIBUTION_FIRST, ATTRIBUTION_SECOND };
    const int lineCount = fits ? 1 : 2;
    int width = 0;
    if (fits) {
        width = normal->textWidth(ATTRIBUTION);
    } else {
        for (const char* line : lines) {
            const int lineWidth = normal->textWidth(line);
            width = lineWidth > width ? lineWidth : width;
        }
    }
    const int y = canvas.height - bottomInset - lineCount * height - 1;
    boxX = 0;
    boxY = y - 1;
    boxWidth = width + 2 * MARGIN;
    boxHeight = lineCount * height + 2;
    canvas.fillRect(boxX, boxY, boxWidth, boxHeight, palette.halo, 215);
    if (fits) {
        normal->drawText(canvas, MARGIN, y, ATTRIBUTION, palette.text);
    } else {
        normal->drawText(canvas, MARGIN, y, lines[0], palette.text);
        normal->drawText(canvas, MARGIN, y + height, lines[1], palette.text);
    }
}

void MapOverlay::drawAttributionMark(Canvas& canvas, const Palette& palette) {
    const int y = canvas.height - bottomInset - MARK_SIZE;
    canvas.fillRect(0, y, MARK_SIZE, MARK_SIZE, palette.halo, 190);
    const int markWidth = normal->textWidth(ATTRIBUTION_MARK);
    normal->drawText(canvas, (MARK_SIZE - markWidth) / 2, y + (MARK_SIZE - normal->lineHeight()) / 2, ATTRIBUTION_MARK, palette.text);
}

bool MapOverlay::hitsAttributionMark(const Canvas& canvas, int x, int y) const {
    return x < MARK_SIZE + 12 && y > canvas.height - bottomInset - MARK_SIZE - 12 && y < canvas.height - bottomInset + 4;
}

int MapOverlay::scaleBarTop(const Canvas& canvas) const {
    return canvas.height - bottomInset - normal->lineHeight() - 8;
}

void MapOverlay::drawScaleBar(Canvas& canvas, double metresPerPixel, const Palette& palette) {
    constexpr int maxBarPixels = 70;
    const double maxMetres = metresPerPixel * maxBarPixels;
    double niceMetres = 1.0;
    while (niceMetres * 10.0 <= maxMetres) {
        niceMetres *= 10.0;
    }
    if (niceMetres * 5.0 <= maxMetres) {
        niceMetres *= 5.0;
    } else if (niceMetres * 2.0 <= maxMetres) {
        niceMetres *= 2.0;
    }
    const int barPixels = static_cast<int>(niceMetres / metresPerPixel + 0.5);
    char label[16];
    if (niceMetres >= 1000.0) {
        snprintf(label, sizeof(label), "%d km", static_cast<int>(niceMetres / 1000.0));
    } else {
        snprintf(label, sizeof(label), "%d m", static_cast<int>(niceMetres));
    }
    const int labelWidth = normal->textWidth(label);
    const int height = normal->lineHeight();
    const int boxWidth = (barPixels > labelWidth ? barPixels : labelWidth) + 2 * MARGIN;
    const int boxX = canvas.width - boxWidth - 1;
    const int boxY = scaleBarTop(canvas);
    canvas.fillRect(boxX, boxY, boxWidth, height + 7, palette.halo, 190);
    normal->drawText(canvas, boxX + (boxWidth - labelWidth) / 2, boxY, label, palette.text);
    const int barX = boxX + (boxWidth - barPixels) / 2;
    const int barY = boxY + height + 3;
    canvas.fillRect(barX, barY, barPixels, 2, palette.text);
    canvas.fillRect(barX, barY - 3, 1, 5, palette.text);
    canvas.fillRect(barX + barPixels - 1, barY - 3, 1, 5, palette.text);
}

void MapOverlay::drawPill(Canvas& canvas, const char* text, const Palette& palette) {
    const int width = normal->textWidth(text) + 4 * MARGIN;
    const int height = normal->lineHeight() + 2;
    const int x = (canvas.width - width) / 2;
    canvas.fillRoundRect(x, 2, width, height, height / 2, palette.text, 200);
    normal->drawText(canvas, x + 2 * MARGIN, 3, text, palette.halo);
}

void MapOverlay::drawToast(Canvas& canvas, const char* text, const UiTheme& theme) {
    constexpr int padding = 8;
    char lines[2][80];
    const int maxWidth = canvas.width - 4 * padding;
    const int count = wrapText(*normal, text, maxWidth, &lines[0][0], sizeof(lines[0]), 2);
    int width = 0;
    for (int i = 0; i < count; i++) {
        width = std::max(width, normal->textWidth(lines[i]));
    }
    const int lineHeight = normal->lineHeight();
    const int boxWidth = width + 2 * padding;
    const int boxHeight = count * lineHeight + padding;
    const int x = (canvas.width - boxWidth) / 2;
    // Above the scale bar and the buttons along the bottom
    const int y = canvas.height - bottomInset - boxHeight - lineHeight - 50;
    canvas.fillRoundRect(x, y, boxWidth, boxHeight, 6, theme.text, 230);
    for (int i = 0; i < count; i++) {
        normal->drawText(canvas, x + padding, y + padding / 2 + i * lineHeight, lines[i], theme.panel);
    }
}

void MapOverlay::drawAccuracyCircle(Canvas& canvas, int x, int y, float radius, const Palette& palette) {
    // Smaller than the marker itself, it would not show
    if (radius < 8.0f) {
        return;
    }
    const int top = std::max(0, static_cast<int>(std::floor(y - radius)));
    const int bottom = std::min(canvas.height - 1, static_cast<int>(std::ceil(y + radius)));
    const float inner = radius - 1.5f;
    for (int row = top; row <= bottom; row++) {
        const float dy = static_cast<float>(row - y);
        const float outerSquared = radius * radius - dy * dy;
        if (outerSquared <= 0.0f) {
            continue;
        }
        const float outerHalf = std::sqrt(outerSquared);
        const float innerSquared = inner * inner - dy * dy;
        const float innerHalf = innerSquared > 0.0f ? std::sqrt(innerSquared) : 0.0f;
        const int left = std::max(0, static_cast<int>(std::ceil(x - outerHalf)));
        const int right = std::min(canvas.width - 1, static_cast<int>(std::floor(x + outerHalf)));
        uint16_t* line = canvas.pixels + row * canvas.width;
        for (int column = left; column <= right; column++) {
            const float dx = std::fabs(static_cast<float>(column - x));
            const bool rim = innerSquared <= 0.0f || dx >= innerHalf;
            line[column] = blendRgb565(palette.marker, line[column], rim ? 18 : 5);
        }
    }
}

void MapOverlay::drawApproximateMarker(Canvas& canvas, int x, int y, float accuracyMetres, const Palette& palette) {
    const float cx = static_cast<float>(x) + 0.5f;
    const float cy = static_cast<float>(y) + 0.5f;
    canvas.fillCircle(cx, cy, 7.5f, palette.text, 70);
    canvas.fillCircle(cx, cy, 6.5f, palette.halo);
    canvas.fillCircle(cx, cy, 4.5f, palette.marker);
    canvas.fillCircle(cx, cy, 2.5f, palette.halo);
    if (std::isnan(accuracyMetres) || accuracyMetres <= 0.0f) {
        return;
    }
    char label[16];
    if (accuracyMetres >= 1000.0f) {
        snprintf(label, sizeof(label), "~%d km", static_cast<int>(accuracyMetres / 1000.0f + 0.5f));
    } else {
        snprintf(label, sizeof(label), "~%d m", static_cast<int>(accuracyMetres + 0.5f));
    }
    // Right of the ring, or left of it near the right edge
    const int width = small->textWidth(label) + 2 * MARGIN;
    const int height = small->lineHeight();
    int boxX = x + 10;
    if (boxX + width > canvas.width) {
        boxX = x - 10 - width;
    }
    const int boxY = y - height / 2;
    canvas.fillRoundRect(boxX, boxY, width, height, 4, palette.halo, 190);
    small->drawText(canvas, boxX + MARGIN, boxY, label, palette.text);
}

void MapOverlay::drawMarker(Canvas& canvas, int x, int y, bool hasCourse, float courseDegrees, const Palette& palette) {
    const float cx = static_cast<float>(x) + 0.5f;
    const float cy = static_cast<float>(y) + 0.5f;
    canvas.fillCircle(cx, cy, 7.5f, palette.text, 70);
    canvas.fillCircle(cx, cy, 6.5f, palette.halo);
    canvas.fillCircle(cx, cy, 4.5f, palette.marker);
    if (!hasCourse) {
        return;
    }
    // A short arrow head ahead of the dot, in the direction of travel
    const float radians = courseDegrees * 3.14159265f / 180.0f;
    const float sx = std::sin(radians);
    const float sy = -std::cos(radians);
    const float tipX = cx + sx * 14.0f;
    const float tipY = cy + sy * 14.0f;
    const float baseX = cx + sx * 8.0f;
    const float baseY = cy + sy * 8.0f;
    canvas.line(baseX - sy * 4.0f, baseY + sx * 4.0f, tipX, tipY, 2.5f, palette.marker);
    canvas.line(baseX + sy * 4.0f, baseY - sx * 4.0f, tipX, tipY, 2.5f, palette.marker);
}

} // namespace maps
