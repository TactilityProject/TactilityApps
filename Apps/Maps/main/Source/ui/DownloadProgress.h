// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "Widgets.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** What the progress pill is reporting: a region download, a region update, or detail tiles being fetched. */
enum class TransferKind : uint8_t {
    Download,
    Update,
    Browse
};

/** "Downloading Bratislava 42% · 120/284 tiles", "Updating Bratislava 42% · 120/284 tiles", "Fetching detail 2/4". */
void formatProgressPill(TransferKind kind, const char* title, int64_t done, int64_t total, char* out, size_t size);

/** Bytes per second from (time, bytes so far) samples, smoothed; restarts when bytes go down. */
class TransferRate {
public:
    void reset();
    void add(uint32_t nowMs, uint64_t bytes);
    /** 0 until two samples at least 1 s apart were seen. */
    double bytesPerSecond() const;
    /** Seconds until totalBytes at the current rate, -1 when unknown. */
    int64_t secondsLeft(uint64_t bytes, uint64_t totalBytes) const;

private:
    bool hasSample = false;
    bool hasRate = false;
    uint32_t lastTimeMs = 0;
    uint64_t lastBytes = 0;
    double rate = 0.0;
};

/** "1.2 MB" for bytes >= 1e6, "340 KB" otherwise (1 KB = 1000 bytes); "4 min 10 s", "35 s", "1 h 05 min". */
void formatBytes(uint64_t bytes, char* out, size_t size);
void formatDuration(int64_t seconds, char* out, size_t size);

/** Details of the download running now, opened by tapping the progress pill. */
class DownloadCard {
public:
    static constexpr int TITLE_LENGTH = 64;
    static constexpr int LINE_LENGTH = 64;
    static constexpr int MAX_LINES = 5;

    enum class Button : uint8_t {
        None,
        PauseResume,
        Cancel,
        Close
    };

    void open() { visible = true; focus = 2; }
    void close() { visible = false; }
    bool isOpen() const { return visible; }
    /** title: region name; lines: up to 5 lines of details; paused decides "Pause" or "Resume". */
    void set(const char* title, const char* const* lines, int lineCount, bool paused);
    void layout(int width, int height, Font& font);
    void draw(Canvas& canvas, Font& font, const UiTheme& theme);
    /** Enter presses the focused button, Tab/arrows/a/d/w/s move the focus, Esc and Backspace give Close. */
    Button handleKey(uint32_t key);
    /** A button, Close for a tap outside the card, None inside it. */
    Button handleTap(int x, int y);

private:
    Button buttonAt(int index) const;

    bool visible = false;
    bool paused = false;
    int focus = 2;
    char title[TITLE_LENGTH] = "";
    char lines[MAX_LINES][LINE_LENGTH] {};
    int lineCount = 0;
    Rect frame {};
    Rect buttons[3] {};
};

} // namespace maps
