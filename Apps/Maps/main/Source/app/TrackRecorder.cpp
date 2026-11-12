// SPDX-License-Identifier: GPL-3.0-or-later
#include "TrackRecorder.h"

#include "MapView.h"

#include "../gpx/GpxWriter.h"
#include "../platform/Platform.h"

#include <sys/stat.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace maps {

namespace {

constexpr size_t PENDING_BYTES = 16 * 1024;
constexpr const char* TRACK_END = "</trkseg></trk>\n</gpx>\n";

/** Whether a track file ends with </gpx>, read from its last bytes. */
bool isClosed(FILE* file) {
    char tail[16] = "";
    if (fseek(file, -static_cast<long>(sizeof(tail) - 1), SEEK_END) != 0) {
        return false;
    }
    const size_t count = fread(tail, 1, sizeof(tail) - 1, file);
    tail[count] = '\0';
    return strstr(tail, "</gpx>") != nullptr;
}

} // namespace

TrackRecorder::~TrackRecorder() {
    largeFree(pending);
    largeFree(drawn);
}

void TrackRecorder::closeUnfinished(IoTask& io, const char* root) {
    char folder[PATH_MAX_LENGTH];
    snprintf(folder, sizeof(folder), "%s/tracks", root);
    auto close = [&] {
        struct Context {
            const char* folder;
            int closed;
        } context { folder, 0 };
        listDirectory(folder, [](const char* fileName, bool isDirectory, void* data) {
            auto* context = static_cast<Context*>(data);
            const size_t length = strlen(fileName);
            if (isDirectory || length < 5 || strcmp(fileName + length - 4, ".gpx") != 0) {
                return true;
            }
            char file[PATH_MAX_LENGTH];
            snprintf(file, sizeof(file), "%s/%s", context->folder, fileName);
            FILE* track = fopen(file, "rb");
            if (track == nullptr) {
                return true;
            }
            const bool closed = isClosed(track);
            // A crash can only have cut a line in the middle: the last whole point is kept, then the end tags
            long keep = 0;
            if (!closed && fseek(track, 0, SEEK_END) == 0) {
                const long size = ftell(track);
                const long window = size < 320 ? size : 320;
                char last[321];
                if (fseek(track, size - window, SEEK_SET) == 0 && fread(last, 1, static_cast<size_t>(window), track) == static_cast<size_t>(window)) {
                    last[window] = '\0';
                    const char* end = nullptr;
                    for (const char* found = strstr(last, "</trkpt>\n"); found != nullptr; found = strstr(found + 1, "</trkpt>\n")) {
                        end = found;
                    }
                    const char* segment = nullptr;
                    for (const char* found = strstr(last, "<trkseg>\n"); found != nullptr; found = strstr(found + 1, "<trkseg>\n")) {
                        segment = found;
                    }
                    if (end != nullptr && (segment == nullptr || end > segment)) {
                        keep = size - window + static_cast<long>(end - last) + 9;
                    } else if (segment != nullptr) {
                        keep = size - window + static_cast<long>(segment - last) + 9;
                    }
                }
            }
            fclose(track);
            if (closed || keep <= 0) {
                return true;
            }
            // Copied up to the last whole point through a .tmp file, then the end tags
            char temporary[PATH_MAX_LENGTH + 8];
            snprintf(temporary, sizeof(temporary), "%s.tmp", file);
            FILE* source = fopen(file, "rb");
            FILE* target = fopen(temporary, "wb");
            bool ok = source != nullptr && target != nullptr;
            char buffer[256];
            long left = keep;
            while (ok && left > 0) {
                const size_t wanted = left < static_cast<long>(sizeof(buffer)) ? static_cast<size_t>(left) : sizeof(buffer);
                ok = fread(buffer, 1, wanted, source) == wanted && fwrite(buffer, 1, wanted, target) == wanted;
                left -= static_cast<long>(wanted);
            }
            ok = ok && fputs(TRACK_END, target) >= 0;
            if (source != nullptr) {
                fclose(source);
            }
            if (target != nullptr) {
                fclose(target);
            }
            if (ok && remove(file) == 0 && rename(temporary, file) == 0) {
                context->closed++;
            } else {
                remove(temporary);
            }
            return true;
        }, &context);
        if (context.closed > 0) {
            logInfo("Closed %d unfinished track file(s)", context.closed);
        }
        return true;
    };
    runOnCard(io, close, IoPriority::Background);
}

bool TrackRecorder::ensureRoom() {
    if (drawnCount < drawnCapacity) {
        return true;
    }
    if (drawnCapacity >= MAX_DRAWN_POINTS) {
        return false;
    }
    const int capacity = drawnCapacity == 0 ? 1024 : (drawnCapacity * 2 < MAX_DRAWN_POINTS ? drawnCapacity * 2 : MAX_DRAWN_POINTS);
    auto* bigger = static_cast<Point*>(largeAlloc(sizeof(Point) * static_cast<size_t>(capacity)));
    if (bigger == nullptr) {
        return false;
    }
    if (drawn != nullptr) {
        memcpy(bigger, drawn, sizeof(Point) * static_cast<size_t>(drawnCount));
        largeFree(drawn);
    }
    drawn = bigger;
    drawnCapacity = capacity;
    return true;
}

bool TrackRecorder::start(IoTask& io, const char* root, uint32_t nowMs) {
    if (currentState != State::Off) {
        return true;
    }
    if (pending == nullptr) {
        pending = static_cast<char*>(largeAlloc(PENDING_BYTES));
        pendingCapacity = pending != nullptr ? PENDING_BYTES : 0;
    }
    if (pending == nullptr) {
        return false;
    }
    // Named by the local start time; without a set clock by a number
    const time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    char folder[PATH_MAX_LENGTH];
    snprintf(folder, sizeof(folder), "%s/tracks", root);
    bool made = false;
    auto create = [&] {
        mkdir(folder, 0777);
        for (int number = 0; number < 100 && !made; number++) {
            if (local.tm_year + 1900 >= 2024) {
                strftime(name, sizeof(name), "%Y%m%d-%H%M", &local);
                if (number > 0) {
                    const size_t length = strlen(name);
                    snprintf(name + length, sizeof(name) - length, "-%d", number);
                }
            } else {
                snprintf(name, sizeof(name), "track%03d", number + 1);
            }
            snprintf(path, sizeof(path), "%s/%s.gpx", folder, name);
            struct stat info {};
            if (stat(path, &info) == 0) {
                continue;
            }
            FILE* file = fopen(path, "wb");
            if (file == nullptr) {
                return false;
            }
            char text[512];
            const int header = gpxHeader(text, sizeof(text), APP_VERSION);
            const int track = header >= 0 ? gpxTrackStart(text + header, sizeof(text) - static_cast<size_t>(header), name) : -1;
            made = track >= 0 && fwrite(text, 1, static_cast<size_t>(header + track), file) == static_cast<size_t>(header + track);
            fclose(file);
        }
        return made;
    };
    if (!runOnCard(io, create, IoPriority::Background)) {
        return false;
    }
    filter.reset();
    drawnCount = 0;
    segmentCount = 0;
    pendingLength = 0;
    newSegment = false;
    startedMs = nowMs;
    pausedMs = 0;
    lastFlushMs = nowMs;
    currentState = State::Recording;
    logInfo("Recording track %s", path);
    return true;
}

void TrackRecorder::pause(uint32_t nowMs) {
    if (currentState == State::Recording) {
        currentState = State::Paused;
        pauseStartMs = nowMs;
    }
}

void TrackRecorder::resume(uint32_t nowMs) {
    if (currentState == State::Paused) {
        currentState = State::Recording;
        pausedMs += nowMs - pauseStartMs;
        // A new segment, so the line does not jump across what was not recorded
        newSegment = true;
        filter.reset();
    }
}

uint32_t TrackRecorder::elapsedMs(uint32_t nowMs) const {
    if (currentState == State::Off) {
        return 0;
    }
    const uint32_t end = currentState == State::Paused ? pauseStartMs : nowMs;
    return end - startedMs - pausedMs;
}

bool TrackRecorder::segmentStartsAt(int index) const {
    for (int i = 0; i < segmentCount; i++) {
        if (segmentStarts[i] == index) {
            return true;
        }
    }
    return false;
}

void TrackRecorder::addFix(const PositionFix& fix) {
    if (currentState != State::Recording || !filter.accept(fix)) {
        return;
    }
    char line[256];
    int used = 0;
    if (newSegment) {
        used = snprintf(line, sizeof(line), "</trkseg><trkseg>\n");
        if (segmentCount < MAX_SEGMENT_STARTS) {
            segmentStarts[segmentCount++] = drawnCount;
        }
        newSegment = false;
    }
    GpxPoint point {};
    point.kind = GpxKind::TrackPoint;
    point.lat = fix.latitude;
    point.lon = fix.longitude;
    point.elevation = NAN;
    point.time = fix.timeMs > 0 ? fix.timeMs / 1000 : static_cast<int64_t>(time(nullptr));
    const int written = gpxTrackPoint(line + used, sizeof(line) - static_cast<size_t>(used), point);
    if (written > 0 && pendingLength + static_cast<size_t>(used + written) <= pendingCapacity) {
        memcpy(pending + pendingLength, line, static_cast<size_t>(used + written));
        pendingLength += static_cast<size_t>(used + written);
    }
    if (ensureRoom()) {
        drawn[drawnCount++] = { static_cast<float>(fix.latitude), static_cast<float>(fix.longitude) };
    }
}

bool TrackRecorder::flush(IoTask& io, const char* closing) {
    if (pendingLength == 0 && closing == nullptr) {
        return true;
    }
    auto append = [&] {
        FILE* file = fopen(path, "ab");
        if (file == nullptr) {
            return false;
        }
        bool ok = pendingLength == 0 || fwrite(pending, 1, pendingLength, file) == pendingLength;
        if (ok && closing != nullptr) {
            ok = fputs(closing, file) >= 0;
        }
        fclose(file);
        return ok;
    };
    const bool ok = runOnCard(io, append, IoPriority::Background);
    if (ok) {
        pendingLength = 0;
    } else {
        logError("Could not write to %s", path);
    }
    return ok;
}

void TrackRecorder::flushIfDue(IoTask& io, uint32_t nowMs) {
    if (currentState != State::Off && nowMs - lastFlushMs >= FLUSH_INTERVAL_MS) {
        lastFlushMs = nowMs;
        flush(io, nullptr);
    }
}

void TrackRecorder::stop(IoTask& io, uint32_t nowMs) {
    if (currentState == State::Off) {
        return;
    }
    (void)nowMs;
    flush(io, TRACK_END);
    logInfo("Track %s stopped: %d points, %.0f m", path, drawnCount, filter.distance());
    currentState = State::Off;
}

} // namespace maps
