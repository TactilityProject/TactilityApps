// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadFrame.h"

#include <cstdio>

namespace maps {

namespace {

constexpr int BUTTON_WIDTH = 92;

} // namespace

DownloadLayout downloadLayout(int width, int height, int lineHeight) {
    DownloadLayout layout {};
    const int barHeight = 2 * lineHeight + 10;
    layout.bar = { 0, height - barHeight, width, barHeight };
    layout.button = { width - BUTTON_WIDTH - 5, layout.bar.y + 5, BUTTON_WIDTH, barHeight - 10 };
    layout.mark = { 0, layout.bar.y, 18, barHeight };
    const int margin = DownloadLayout::MARGIN;
    layout.frame = { margin, margin, width - 2 * margin, layout.bar.y - 2 * margin };
    return layout;
}

BoundingBox screenBounds(const Rect& rect, double centerX, double centerY, int zoom, int width, int height) {
    const double left = centerX + (rect.x - width / 2.0);
    const double right = left + rect.width;
    const double top = centerY + (rect.y - height / 2.0);
    const double bottom = top + rect.height;
    return { worldXToLon(left, zoom), worldYToLat(bottom, zoom), worldXToLon(right, zoom), worldYToLat(top, zoom) };
}

void describePlan(const DownloadPlan& plan, char* first, size_t firstSize, char* second, size_t secondSize) {
    if (!plan.fits) {
        snprintf(first, firstSize, "Too large to download");
        snprintf(second, secondSize, "Zoom in to cover less (I)");
        return;
    }
    const auto megabytes = static_cast<int>((plan.estimatedBytes + 500000) / 1000000);
    if (megabytes > 0) {
        snprintf(first, firstSize, "%d tiles, ~%d MB", static_cast<int>(plan.tiles), megabytes);
    } else {
        snprintf(first, firstSize, "%d tiles, <1 MB", static_cast<int>(plan.tiles));
    }
    if (plan.maxZoom < MAX_SOURCE_ZOOM) {
        snprintf(second, secondSize, "Max zoom %d to fit", plan.maxZoom);
    } else {
        snprintf(second, secondSize, "Max zoom %d", plan.maxZoom);
    }
}

void drawDownloadScreen(Canvas& canvas, Font& font, const UiTheme& theme, const DownloadLayout& layout, const DownloadPlan& plan) {
    // Outside the frame the map stays dimmed, the frame itself never moves
    const Rect& f = layout.frame;
    const Rect& bar = layout.bar;
    canvas.fillRect(0, 0, canvas.width, f.y, theme.scrim, 110);
    canvas.fillRect(0, f.y + f.height, canvas.width, bar.y - f.y - f.height, theme.scrim, 110);
    canvas.fillRect(0, f.y, f.x, f.height, theme.scrim, 110);
    canvas.fillRect(f.x + f.width, f.y, canvas.width - f.x - f.width, f.height, theme.scrim, 110);
    canvas.frameRect(f.x - 2, f.y - 2, f.width + 4, f.height + 4, theme.accent, 2);

    char first[48];
    char second[48];
    describePlan(plan, first, sizeof(first), second, sizeof(second));
    canvas.fillRect(bar.x, bar.y, bar.width, bar.height, theme.panel);
    canvas.fillRect(bar.x, bar.y, bar.width, 1, theme.border);
    // The (c) mark lives in the info bar here, the screen has nothing else
    const Rect& mark = layout.mark;
    const int markWidth = font.textWidth("\xC2\xA9");
    font.drawText(canvas, mark.x + (mark.width - markWidth) / 2, mark.y + (mark.height - font.lineHeight()) / 2, "\xC2\xA9", theme.text);
    const int textX = mark.x + mark.width + 2;
    const int textWidth = layout.button.x - textX - 4;
    drawClippedText(canvas, font, textX, bar.y + 4, first, textWidth, theme.text);
    drawClippedText(canvas, font, textX, bar.y + 5 + font.lineHeight(), second, textWidth, plan.fits ? theme.muted : theme.danger);
    if (plan.fits) {
        drawButton(canvas, font, theme, layout.button, "Download", true);
    } else {
        drawButton(canvas, font, theme, layout.button, "Download", false);
    }
}

} // namespace maps
