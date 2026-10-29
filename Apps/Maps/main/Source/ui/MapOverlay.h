// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Canvas.h"

#include <cstdint>

namespace maps {

/** What is drawn over the map itself: attribution, scale bar, position, progress pill, toasts. */
class MapOverlay {
public:
    static const char* attributionText();

    void setFonts(Font* normalFont, Font* smallFont) {
        normal = normalFont;
        small = smallFont != nullptr ? smallFont : normalFont;
    }
    Font& font() { return *normal; }
    Font& smallFont() { return *small; }

    /** Space at the bottom of the screen taken by a panel, the attribution and scale bar sit above it. */
    void setBottomInset(int inset) { bottomInset = inset; }

    /** The full attribution along the bottom edge. Returns its box, which opens About and licences when tapped. */
    void drawAttribution(Canvas& canvas, const Palette& palette, int& boxX, int& boxY, int& boxWidth, int& boxHeight);
    /** The collapsed attribution, a small © mark in the bottom left corner. */
    void drawAttributionMark(Canvas& canvas, const Palette& palette);
    /** Whether a screen position hits the © mark, a little larger than drawn so a finger finds it. */
    bool hitsAttributionMark(const Canvas& canvas, int x, int y) const;
    /** The top of the scale bar box, buttons above it keep clear of it. */
    int scaleBarTop(const Canvas& canvas) const;
    void drawScaleBar(Canvas& canvas, double metresPerPixel, const Palette& palette);
    /** A small rounded message at the top of the screen, for progress. */
    void drawPill(Canvas& canvas, const char* text, const Palette& palette);
    /** A short message of up to two lines above the bottom edge. */
    void drawToast(Canvas& canvas, const char* text, const UiTheme& theme);
    void drawMarker(Canvas& canvas, int x, int y, bool hasCourse, float courseDegrees, const Palette& palette);
    /** A translucent disc with a thin rim, how far off the position may be. */
    void drawAccuracyCircle(Canvas& canvas, int x, int y, float radius, const Palette& palette);
    /**
     * A coarse position, as from an IP lookup: a hollow ring with a small "~25 km" label instead of an accuracy
     * circle, which would cover the whole screen. accuracyMetres may be NAN.
     */
    void drawApproximateMarker(Canvas& canvas, int x, int y, float accuracyMetres, const Palette& palette);

    static constexpr int MARK_SIZE = 18;
    static constexpr int MARGIN = 3;

private:
    Font* normal = nullptr;
    Font* small = nullptr;
    int bottomInset = 0;
};

} // namespace maps
