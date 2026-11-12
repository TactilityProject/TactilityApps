// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Widgets.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace maps {

/** Kinds of work the app does in the background, each shown as one line in the work card. */
enum class WorkKind : uint8_t {
    PrepareTiles,    // "Preparing tiles"
    UpgradeTiles,    // "Upgrading tiles"
    IdleUpdate,      // "Updating maps"
    Fonts,           // "Making label fonts"
    Catalog,         // "Loading the region list"
    CacheCleanup,    // "Cleaning the cache"
    GpxImport,       // "Importing GPX"
    MoveBrowsed,     // "Moving browsed tiles"
    AddressIndex,    // "Building search index" (M6)
    SplitTiles,      // "Splitting tiles"
    Count
};

/**
 * What runs in the background, set by the tasks doing it and read by the map loop. Each kind has a running flag and
 * optional done/total counts. Writers and the reader may be different tasks: every field is a std::atomic (relaxed
 * is enough), and readers accept a slightly stale count.
 */
class BackgroundWork {
public:
    void start(WorkKind kind, int32_t total = 0);   // running, done = 0
    void progress(WorkKind kind, int32_t done, int32_t total = -1);  // total -1 keeps it
    void finish(WorkKind kind);
    bool isRunning(WorkKind kind) const;
    bool any() const;
    /** Increments on every start/finish (not on progress), so the loop knows when to redraw the dot. */
    uint32_t generation() const;
    /** One line, e.g. "Upgrading tiles 120/480", or "Making label fonts" without counts (total <= 0). */
    void describe(WorkKind kind, char* out, size_t size) const;
    /** The lines of every running kind, in enum order; returns how many (at most maxLines). */
    int lines(char (*out)[48], int maxLines) const;

private:
    struct Entry {
        std::atomic<bool> running{false};
        std::atomic<int32_t> done{0};
        std::atomic<int32_t> total{0};
    };

    Entry entries[static_cast<size_t>(WorkKind::Count)]{};
    std::atomic<uint32_t> gen{0};
};

/**
 * The small dot in the top-left corner while work runs. Amber (0xFD20 in RGB565), about 6 px across, at (x, y) its
 * centre. Solid, without animation: it is either shown or not.
 */
void drawWorkDot(Canvas& canvas, int x, int y, uint16_t color);

/** The dot's touch target, bigger than the dot (at least 24 x 24 px), for hit testing. */
Rect workDotTarget(int x, int y);

/** Details of the background work, following DownloadCard's style. */
class WorkCard {
public:
    static constexpr int LINE_LENGTH = 48;
    static constexpr int MAX_LINES = 5;

    void open();
    void close();
    bool isOpen() const { return visible; }
    void set(const BackgroundWork& work);
    void layout(int width, int height, Font& font);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);
    bool hitClose(int x, int y) const;
    Rect closeButton() const;

private:
    bool visible = false;
    char lines[MAX_LINES][LINE_LENGTH] {};
    int lineCount = 0;
    Rect frame {};
    Rect closeBtn {};
};

} // namespace maps
