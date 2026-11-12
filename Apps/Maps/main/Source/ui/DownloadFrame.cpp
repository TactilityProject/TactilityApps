// SPDX-License-Identifier: GPL-3.0-or-later
#include "DownloadFrame.h"

#include <algorithm>
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
    layout.back = { margin + 4, margin + 4, 36, 36 };
    return layout;
}

BoundingBox screenBounds(const Rect& rect, double centerX, double centerY, int zoom, int width, int height) {
    const double left = centerX + (rect.x - width / 2.0);
    const double right = left + rect.width;
    const double top = centerY + (rect.y - height / 2.0);
    const double bottom = top + rect.height;
    return { worldXToLon(left, zoom), worldYToLat(bottom, zoom), worldXToLon(right, zoom), worldYToLat(top, zoom) };
}

void describePlan(const DownloadPlan& plan, char* first, size_t firstSize, char* second, size_t secondSize, int64_t onCard) {
    if (!plan.fits) {
        snprintf(first, firstSize, "Too large to download");
        snprintf(second, secondSize, "Zoom in to cover less");
        return;
    }
    if (onCard > 0 && plan.tiles > 0) {
        // Browsed tiles move into the region: only the rest is fetched
        const int64_t fetched = plan.tiles - std::min(onCard, plan.tiles);
        const auto megabytes = static_cast<int>((plan.estimatedBytes * static_cast<uint64_t>(fetched) / static_cast<uint64_t>(plan.tiles) + 500000) / 1000000);
        snprintf(first, firstSize, "%d tiles, %d already on the card", static_cast<int>(plan.tiles), static_cast<int>(std::min(onCard, plan.tiles)));
        if (megabytes > 0) {
            snprintf(second, secondSize, "~%d MB to fetch, max zoom %d", megabytes, plan.maxZoom);
        } else {
            snprintf(second, secondSize, "<1 MB to fetch, max zoom %d", plan.maxZoom);
        }
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

void drawDownloadScreen(Canvas& canvas, Font& font, const UiTheme& theme, const DownloadLayout& layout, const DownloadPlan& plan,
    int64_t onCard) {
    // Outside the frame the map stays dimmed, the frame itself never moves
    const Rect& f = layout.frame;
    const Rect& bar = layout.bar;
    canvas.fillRect(0, 0, canvas.width, f.y, theme.scrim, 110);
    canvas.fillRect(0, f.y + f.height, canvas.width, bar.y - f.y - f.height, theme.scrim, 110);
    canvas.fillRect(0, f.y, f.x, f.height, theme.scrim, 110);
    canvas.fillRect(f.x + f.width, f.y, canvas.width - f.x - f.width, f.height, theme.scrim, 110);
    canvas.frameRect(f.x - 2, f.y - 2, f.width + 4, f.height + 4, theme.accent, 2);
    // Back to the map: a round button with the lists' < sign
    const Rect& back = layout.back;
    canvas.fillRoundRect(back.x, back.y, back.width, back.height, back.width / 2, theme.panel, theme.panelOpacity);
    const auto backX = static_cast<float>(back.x + back.width / 2);
    const auto backY = static_cast<float>(back.y + back.height / 2);
    canvas.line(backX + 3.0f, backY - 7.0f, backX - 4.0f, backY, 2.5f, theme.text);
    canvas.line(backX - 4.0f, backY, backX + 3.0f, backY + 7.0f, 2.5f, theme.text);

    char first[48];
    char second[48];
    describePlan(plan, first, sizeof(first), second, sizeof(second), onCard);
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
