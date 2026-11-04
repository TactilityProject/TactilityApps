// SPDX-License-Identifier: GPL-3.0-or-later
#include "GpxLayer.h"

#include "../platform/Platform.h"
#include "../render/PreparedTile.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

constexpr uint32_t MAGIC = 0x4347'4D54; // "TMGC"
constexpr size_t HEADER_BYTES = 4 * 6;
constexpr size_t LINE_BYTES = 4 * 6 + 1;
constexpr size_t PIN_BYTES = 8 + 32;

uint32_t readU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24;
}

void putU32(ByteWriter& out, uint32_t value) {
    out.putU32(value);
}

} // namespace

GpxLayer::~GpxLayer() {
    clear();
}

void GpxLayer::clear() {
    largeFree(lineTable);
    largeFree(pointTable);
    largeFree(pinTable);
    lineTable = nullptr;
    pointTable = nullptr;
    pinTable = nullptr;
    lines = 0;
    pins = 0;
}

bool GpxLayer::load(const uint8_t* data, size_t size, uint32_t sourceSize) {
    clear();
    if (size < HEADER_BYTES || readU32(data) != MAGIC || readU32(data + 4) != CACHE_VERSION || readU32(data + 8) != sourceSize) {
        return false;
    }
    const uint32_t lineTotal = readU32(data + 12);
    const uint32_t pointTotal = readU32(data + 16);
    const uint32_t pinTotal = readU32(data + 20);
    const size_t needed = HEADER_BYTES + lineTotal * LINE_BYTES + static_cast<size_t>(pointTotal) * 8 + pinTotal * PIN_BYTES;
    if (lineTotal > static_cast<uint32_t>(GpxCacheBuilder::MAX_LINES) || pointTotal > static_cast<uint32_t>(GpxCacheBuilder::MAX_POINTS) ||
        pinTotal > static_cast<uint32_t>(GpxCacheBuilder::MAX_PINS) || needed != size) {
        return false;
    }
    lineTable = static_cast<Line*>(largeAlloc(sizeof(Line) * (lineTotal > 0 ? lineTotal : 1)));
    pointTable = static_cast<uint32_t*>(largeAlloc(sizeof(uint32_t) * 2 * (pointTotal > 0 ? pointTotal : 1)));
    pinTable = static_cast<Pin*>(largeAlloc(sizeof(Pin) * (pinTotal > 0 ? pinTotal : 1)));
    if (lineTable == nullptr || pointTable == nullptr || pinTable == nullptr) {
        clear();
        return false;
    }
    const uint8_t* p = data + HEADER_BYTES;
    for (uint32_t i = 0; i < lineTotal; i++, p += LINE_BYTES) {
        Line& line = lineTable[i];
        line.kind = static_cast<GpxKind>(p[0]);
        line.first = readU32(p + 1);
        line.count = readU32(p + 5);
        line.minX = readU32(p + 9);
        line.minY = readU32(p + 13);
        line.maxX = readU32(p + 17);
        line.maxY = readU32(p + 21);
        if (line.first > pointTotal || line.count > pointTotal - line.first) {
            clear();
            return false;
        }
    }
    for (uint32_t i = 0; i < pointTotal * 2; i++, p += 4) {
        pointTable[i] = readU32(p);
    }
    for (uint32_t i = 0; i < pinTotal; i++, p += PIN_BYTES) {
        pinTable[i].x = readU32(p);
        pinTable[i].y = readU32(p + 4);
        memcpy(pinTable[i].name, p + 8, 32);
        pinTable[i].name[31] = '\0';
    }
    lines = static_cast<int>(lineTotal);
    pins = static_cast<int>(pinTotal);
    return true;
}

bool GpxLayer::bounds(BoundingBox& box) const {
    if (lines == 0 && pins == 0) {
        return false;
    }
    uint32_t minX = UINT32_MAX;
    uint32_t minY = UINT32_MAX;
    uint32_t maxX = 0;
    uint32_t maxY = 0;
    for (int i = 0; i < lines; i++) {
        minX = std::min(minX, lineTable[i].minX);
        minY = std::min(minY, lineTable[i].minY);
        maxX = std::max(maxX, lineTable[i].maxX);
        maxY = std::max(maxY, lineTable[i].maxY);
    }
    for (int i = 0; i < pins; i++) {
        minX = std::min(minX, pinTable[i].x);
        minY = std::min(minY, pinTable[i].y);
        maxX = std::max(maxX, pinTable[i].x);
        maxY = std::max(maxY, pinTable[i].y);
    }
    box = { worldXToLon(minX, CACHE_ZOOM), worldYToLat(maxY, CACHE_ZOOM), worldXToLon(maxX, CACHE_ZOOM), worldYToLat(minY, CACHE_ZOOM) };
    return true;
}

// ---------------------------------------------------------------------------------------------------------------

GpxCacheBuilder::GpxCacheBuilder() : reader(*this) {
    lineTable = static_cast<GpxLayer::Line*>(largeAlloc(sizeof(GpxLayer::Line) * MAX_LINES));
    pointTable = static_cast<uint32_t*>(largeAlloc(sizeof(uint32_t) * 2 * MAX_POINTS));
    pinTable = static_cast<GpxLayer::Pin*>(largeAlloc(sizeof(GpxLayer::Pin) * MAX_PINS));
    allocated = lineTable != nullptr && pointTable != nullptr && pinTable != nullptr;
}

GpxCacheBuilder::~GpxCacheBuilder() {
    largeFree(lineTable);
    largeFree(pointTable);
    largeFree(pinTable);
}

bool GpxCacheBuilder::feed(const char* data, size_t size) {
    return allocated && reader.feed(data, size);
}

const char* GpxCacheBuilder::error() const {
    if (!allocated) {
        return "Not enough memory";
    }
    if (reader.error()[0] != '\0') {
        return reader.error();
    }
    return lines == 0 && pins == 0 ? "No tracks, routes or waypoints" : "";
}

void GpxCacheBuilder::openLine(GpxKind kind) {
    if (lines >= MAX_LINES) {
        lineOpen = false;
        return;
    }
    GpxLayer::Line& line = lineTable[lines++];
    line = { kind, static_cast<uint32_t>(pointCount), 0, UINT32_MAX, UINT32_MAX, 0, 0 };
    lineOpen = true;
    openKind = kind;
}

void GpxCacheBuilder::beginLine(GpxKind kind, const char* name) {
    (void)name;
    openLine(kind == GpxKind::RoutePoint ? GpxKind::RoutePoint : GpxKind::TrackPoint);
}

void GpxCacheBuilder::beginSegment() {
    // Each segment is its own line: no stroke across the gap between segments
    if (!lineOpen || lineTable[lines - 1].count > 0) {
        openLine(GpxKind::TrackPoint);
    }
}

void GpxCacheBuilder::point(const GpxPoint& point) {
    pointsSeen++;
    const auto x = static_cast<uint32_t>(lonToWorldX(point.lon, GpxLayer::CACHE_ZOOM));
    const auto y = static_cast<uint32_t>(latToWorldY(point.lat, GpxLayer::CACHE_ZOOM));
    if (point.kind == GpxKind::Waypoint) {
        if (pins < MAX_PINS) {
            GpxLayer::Pin& pin = pinTable[pins++];
            pin.x = x;
            pin.y = y;
            snprintf(pin.name, sizeof(pin.name), "%s", point.name);
        }
        return;
    }
    if (!lineOpen) {
        openLine(point.kind);
    }
    if (!lineOpen || pointCount >= MAX_POINTS) {
        return;
    }
    GpxLayer::Line& line = lineTable[lines - 1];
    // One point per pixel at zoom 18 (about 0.6 m) is all any view needs
    if (line.count > 0) {
        const uint32_t lastX = pointTable[2 * (pointCount - 1)];
        const uint32_t lastY = pointTable[2 * (pointCount - 1) + 1];
        if (lastX == x && lastY == y) {
            return;
        }
    }
    pointTable[2 * pointCount] = x;
    pointTable[2 * pointCount + 1] = y;
    pointCount++;
    line.count++;
    line.minX = std::min(line.minX, x);
    line.minY = std::min(line.minY, y);
    line.maxX = std::max(line.maxX, x);
    line.maxY = std::max(line.maxY, y);
}

void GpxCacheBuilder::endLine() {
    lineOpen = false;
}

bool GpxCacheBuilder::finish(uint32_t sourceSize, ByteWriter& out) {
    if (!allocated) {
        return false;
    }
    // A cut-off file still shows what was read before the cut, a file that is not GPX shows nothing
    const bool complete = reader.finish();
    // Empty lines (a <trk> without points) are left out
    int kept = 0;
    for (int i = 0; i < lines; i++) {
        if (lineTable[i].count >= 2) {
            lineTable[kept++] = lineTable[i];
        }
    }
    lines = kept;
    if ((!complete && pointCount == 0 && pins == 0) || (lines == 0 && pins == 0)) {
        return false;
    }
    out.clear();
    putU32(out, MAGIC);
    putU32(out, GpxLayer::CACHE_VERSION);
    putU32(out, sourceSize);
    putU32(out, static_cast<uint32_t>(lines));
    putU32(out, static_cast<uint32_t>(pointCount));
    putU32(out, static_cast<uint32_t>(pins));
    for (int i = 0; i < lines; i++) {
        const GpxLayer::Line& line = lineTable[i];
        out.put(static_cast<uint8_t>(line.kind));
        putU32(out, line.first);
        putU32(out, line.count);
        putU32(out, line.minX);
        putU32(out, line.minY);
        putU32(out, line.maxX);
        putU32(out, line.maxY);
    }
    for (int i = 0; i < pointCount * 2; i++) {
        putU32(out, pointTable[i]);
    }
    for (int i = 0; i < pins; i++) {
        putU32(out, pinTable[i].x);
        putU32(out, pinTable[i].y);
        char name[32] {};
        snprintf(name, sizeof(name), "%s", pinTable[i].name);
        out.putBytes(name, sizeof(name));
    }
    return !out.failed();
}

} // namespace maps
