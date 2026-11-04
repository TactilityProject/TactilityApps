// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../gpx/GpxReader.h"
#include "../tiles/TileMath.h"

#include <cstddef>
#include <cstdint>

namespace maps {

class ByteWriter;

/**
 * An imported GPX file in the compact form the map draws: tracks and routes as lines of world pixels at zoom 18,
 * thinned to one point per pixel there, and waypoints with their names. Made once from the GPX file by
 * GpxCacheBuilder, stored next to it as <name>.gpc, read back with load().
 */
class GpxLayer {
public:
    static constexpr int CACHE_ZOOM = 18;
    static constexpr uint32_t CACHE_VERSION = 1;

    struct Line {
        GpxKind kind;
        uint32_t first;
        uint32_t count;
        /** World pixels at CACHE_ZOOM. */
        uint32_t minX;
        uint32_t minY;
        uint32_t maxX;
        uint32_t maxY;
    };

    struct Pin {
        uint32_t x;
        uint32_t y;
        char name[32];
    };

    GpxLayer() = default;
    ~GpxLayer();
    GpxLayer(const GpxLayer&) = delete;
    GpxLayer& operator=(const GpxLayer&) = delete;

    /** Reads a cache file's bytes; sourceSize must match the GPX file it was made from, else it is stale. */
    bool load(const uint8_t* data, size_t size, uint32_t sourceSize);
    void clear();

    int lineCount() const { return lines; }
    const Line& line(int index) const { return lineTable[index]; }
    /** The points of all lines, x then y, world pixels at CACHE_ZOOM. */
    const uint32_t* points() const { return pointTable; }
    int pinCount() const { return pins; }
    const Pin& pin(int index) const { return pinTable[index]; }
    /** The area all of it covers, in degrees; false when it is empty. */
    bool bounds(BoundingBox& box) const;

private:
    Line* lineTable = nullptr;
    int lines = 0;
    uint32_t* pointTable = nullptr;
    Pin* pinTable = nullptr;
    int pins = 0;
};

/**
 * Builds a GpxLayer cache from a GPX file fed in chunks of any size: a GpxListener that keeps no points beyond the
 * cache it writes. Limits keep huge files bounded: at most MAX_POINTS points (after thinning), MAX_LINES lines and
 * MAX_PINS waypoints, the rest is left out and counted.
 */
class GpxCacheBuilder final : public GpxListener {
public:
    /** 512 KB of PSRAM while a file is read; points beyond it are left out. */
    static constexpr int MAX_POINTS = 65536;
    static constexpr int MAX_LINES = 256;
    static constexpr int MAX_PINS = 256;

    GpxCacheBuilder();
    ~GpxCacheBuilder() override;
    GpxCacheBuilder(const GpxCacheBuilder&) = delete;
    GpxCacheBuilder& operator=(const GpxCacheBuilder&) = delete;

    /** Feeds GPX bytes; false once the file is known to be bad. */
    bool feed(const char* data, size_t size);
    /** Ends the file and writes the cache into out. False with error() when it was not a usable GPX file. */
    bool finish(uint32_t sourceSize, ByteWriter& out);
    const char* error() const;
    int pointsKept() const { return pointCount; }
    int pointsRead() const { return pointsSeen; }

    void beginLine(GpxKind kind, const char* name) override;
    void beginSegment() override;
    void point(const GpxPoint& point) override;
    void endLine() override;

private:
    void openLine(GpxKind kind);

    GpxReader reader;
    GpxLayer::Line* lineTable;
    uint32_t* pointTable;
    GpxLayer::Pin* pinTable;
    int lines = 0;
    int pointCount = 0;
    int pins = 0;
    int pointsSeen = 0;
    bool lineOpen = false;
    GpxKind openKind = GpxKind::TrackPoint;
    bool allocated = false;
};

} // namespace maps
