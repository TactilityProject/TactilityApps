// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/DownloadPlan.h"
#include "../tiles/TileMath.h"
#include "Widgets.h"

#include <cstddef>

namespace maps {

/**
 * The Download area screen: a fixed frame centred in the map area above the info bar, with an even margin.
 * The map moves under it, so panning picks the place and zooming how much ground it covers.
 */
struct DownloadLayout {
    static constexpr int MARGIN = 10;

    Rect frame;
    Rect bar;
    Rect button;
    /** A round < in the frame's top left corner: back to the map without downloading (touch screens have no Esc). */
    Rect back;
    /** The (c) mark at the left end of the info bar. */
    Rect mark;
};

DownloadLayout downloadLayout(int width, int height, int lineHeight);

/** The ground under a screen rectangle, for a view centred on centerX, centerY (world pixels at zoom). */
BoundingBox screenBounds(const Rect& rect, double centerX, double centerY, int zoom, int width, int height);

/** The two lines of the info bar for a plan. */
/**
 * The two lines under the download frame. onCard: tiles of the plan already in the browsed region (they move into the
 * region instead of being downloaded), -1 while unknown.
 */
void describePlan(const DownloadPlan& plan, char* first, size_t firstSize, char* second, size_t secondSize, int64_t onCard = -1);

/** Dims the map outside the frame and draws the info bar with the plan and the Download button. */
void drawDownloadScreen(Canvas& canvas, Font& font, const UiTheme& theme, const DownloadLayout& layout, const DownloadPlan& plan,
    int64_t onCard = -1);

} // namespace maps
