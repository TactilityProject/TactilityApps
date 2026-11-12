// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "IoTask.h"

#include "../gpx/Track.h"
#include "../platform/Position.h"
#include "../tiles/TilePaths.h"

#include <cstdint>

namespace maps {

/**
 * Records the user's track into tracks/YYYYMMDD-HHMM.gpx in the app's folder, only while the app is open. Points come
 * from the position updates through TrackFilter, are buffered, and appended to the file every 30 s, so a crash loses
 * at most 30 s; a file left open by a crash is closed the next time the app starts.
 */
class TrackRecorder {
public:
    enum class State : uint8_t {
        Off,
        Recording,
        Paused
    };

    /** A recorded point for drawing, in degrees; floats are good to about half a metre. */
    struct Point {
        float lat;
        float lon;
    };

    static constexpr int MAX_DRAWN_POINTS = 20000;
    static constexpr uint32_t FLUSH_INTERVAL_MS = 30000;

    ~TrackRecorder();
    /** Closes track files a crash left without their end tags. Call once at start, on the I/O task's side. */
    static void closeUnfinished(IoTask& io, const char* root);

    bool start(IoTask& io, const char* root, uint32_t nowMs);
    void pause(uint32_t nowMs);
    void resume(uint32_t nowMs);
    void stop(IoTask& io, uint32_t nowMs);
    /** A new position; stored when it moved far enough (TrackFilter). */
    void addFix(const PositionFix& fix);
    /** Appends the buffered points once 30 s have passed since the last write. */
    void flushIfDue(IoTask& io, uint32_t nowMs);

    State state() const { return currentState; }
    bool isActive() const { return currentState != State::Off; }
    /** Time spent recording, pauses left out. */
    uint32_t elapsedMs(uint32_t nowMs) const;
    double distanceMetres() const { return filter.distance(); }
    const Point* points() const { return drawn; }
    int pointCount() const { return drawnCount; }
    /** Where a new segment starts in points(), after a pause. */
    bool segmentStartsAt(int index) const;
    const char* fileName() const { return name; }

private:
    bool flush(IoTask& io, const char* closing);
    bool ensureRoom();

    State currentState = State::Off;
    TrackFilter filter;
    char path[PATH_MAX_LENGTH] = "";
    char name[24] = "";
    uint32_t startedMs = 0;
    uint32_t pausedMs = 0;
    uint32_t pauseStartMs = 0;
    uint32_t lastFlushMs = 0;
    /** GPX text not yet on the card. */
    char* pending = nullptr;
    size_t pendingLength = 0;
    size_t pendingCapacity = 0;
    Point* drawn = nullptr;
    int drawnCount = 0;
    int drawnCapacity = 0;
    static constexpr int MAX_SEGMENT_STARTS = 32;
    int segmentStarts[MAX_SEGMENT_STARTS] {};
    int segmentCount = 0;
    bool newSegment = false;
};

} // namespace maps
