// SPDX-License-Identifier: GPL-3.0-or-later
#include "TileRenderer.h"

#include "../platform/Platform.h"
#include "PreparedTile.h"

#include <cmath>

namespace maps {

namespace {

/** Thinner lines get no round joins or caps, they would not show. */
constexpr float MIN_JOIN_WIDTH = 1.5f;
/** Cosine of the smallest turn that gets a round join, about 8 degrees. */
constexpr float JOIN_COSINE = 0.99f;
/** Buildings smaller than this, in pixels both ways, are left out at native z14. */
constexpr float MIN_BUILDING_PIXELS = 2.0f;

} // namespace

TileRenderer::~TileRenderer() {
    largeFree(body);
}

bool TileRenderer::init() {
    return decoder.init() && raster.init(TILE_SIZE, TILE_SIZE);
}

void TileRenderer::begin(const TileKey& source, const TileKey& target, const Palette& tilePalette, uint16_t* out) {
    palette = &tilePalette;
    pixels = out;
    zoom = target.z;
    featureCount = 0;
    geometryUs = 0;
    fillUs = 0;
    buildingUs = 0;
    for (int i = 0; i < TILE_SIZE * TILE_SIZE; i++) {
        pixels[i] = palette->background;
    }
    raster.clear();
    cancelled = false;
    dz = target.z - source.z;
    offsetX = static_cast<float>((target.x - (source.x << dz)) * TILE_SIZE);
    offsetY = static_cast<float>((target.y - (source.y << dz)) * TILE_SIZE);
}

void TileRenderer::finishStats(RenderStats* stats, uint32_t start, uint32_t scanned, uint32_t sweeps) const {
    if (stats == nullptr) {
        return;
    }
    stats->totalMs = nowMs() - start;
    stats->scanMs = scanned - start;
    stats->sweeps = sweeps;
    stats->features = featureCount;
    stats->geometryMs = static_cast<uint32_t>(geometryUs / 1000);
    stats->fillMs = static_cast<uint32_t>(fillUs / 1000);
    stats->geometryUs = static_cast<uint32_t>(geometryUs);
    stats->fillUs = static_cast<uint32_t>(fillUs);
    stats->buildingUs = static_cast<uint32_t>(buildingUs);
}

bool TileRenderer::render(ByteInput* input, const TileKey& source, const TileKey& target, const Palette& tilePalette, uint16_t* out, RenderStats* stats) {
    const uint32_t start = nowMs();
    preparedExtent = 0;
    begin(source, target, tilePalette, out);

    if (!decoder.scan(input)) {
        logError("Tile %d/%d/%d could not be read", source.z, source.x, source.y);
        return false;
    }
    const uint32_t scanned = nowMs();

    // Stages are drawn in order. A sweep reads the tile once and draws consecutive stages as long as their
    // layers come later in the file than the layer of the previous stage of that sweep.
    uint32_t sweeps = 0;
    int next = 0;
    constexpr int stageCount = static_cast<int>(Stage::Count);
    while (next < stageCount) {
        Stage sweepStages[stageCount];
        Layer sweepLayers[stageCount];
        int count = 0;
        int lastIndex = -1;
        while (next < stageCount) {
            const auto candidate = static_cast<Stage>(next);
            const Layer layer = stageLayer(candidate);
            const int index = decoder.fileIndex(layer);
            if (index < 0) {
                next++;
                continue;
            }
            if (index <= lastIndex) {
                break;
            }
            sweepStages[count] = candidate;
            sweepLayers[count] = layer;
            count++;
            lastIndex = index;
            next++;
        }
        if (count == 0) {
            break;
        }
        // One sweep visits each layer once, so its stages all use different layers
        for (int i = 0; i < count; i++) {
            layerStages[static_cast<int>(sweepLayers[i])] = sweepStages[i];
        }
        if (!decoder.sweep(sweepLayers, count, *this)) {
            if (!cancelled) {
                logError("Tile %d/%d/%d is damaged", source.z, source.x, source.y);
            }
            return false;
        }
        sweeps++;
    }

    finishStats(stats, start, scanned, sweeps);
    if (stats != nullptr) {
        stats->decompressedBytes = static_cast<uint32_t>(decoder.decompressedSize());
    }
    return true;
}

namespace {

bool readVarint(const uint8_t*& data, const uint8_t* end, uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64 && data < end; shift += 7) {
        const uint8_t byte = *data++;
        value |= static_cast<uint64_t>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return true;
        }
    }
    return false;
}

int32_t unzigzag(uint64_t value) {
    return static_cast<int32_t>((value >> 1) ^ (~(value & 1) + 1));
}

} // namespace

bool TileRenderer::renderPrepared(const uint8_t* data, size_t size, const TileKey& target, const Palette& tilePalette, uint16_t* out, RenderStats* stats) {
    const uint32_t start = nowMs();
    PreparedHeader header {};
    if (!readPreparedHeader(data, size, header) || header.extent <= 0 || PREPARED_HEADER_SIZE + header.storedSize > size) {
        logError("Prepared tile for %d/%d/%d is damaged", target.z, target.x, target.y);
        return false;
    }
    if (target.z < header.tile.z || !(ancestorOf(target, header.tile.z) == header.tile)) {
        return false;
    }
    const uint8_t* bodyStart = data + PREPARED_HEADER_SIZE;
    if ((header.flags & PREPARED_FLAG_DEFLATED) != 0) {
        if (header.bodySize > bodyCapacity) {
            largeFree(body);
            bodyCapacity = (header.bodySize + 0xFFFF) & ~static_cast<size_t>(0xFFFF);
            body = static_cast<uint8_t*>(largeAlloc(bodyCapacity));
            if (body == nullptr) {
                bodyCapacity = 0;
                return false;
            }
        }
        MemoryInput stored(bodyStart, header.storedSize);
        if (!decoder.inflate(&stored, body, header.bodySize)) {
            logError("Prepared tile %d/%d/%d does not inflate", header.tile.z, header.tile.x, header.tile.y);
            return false;
        }
        bodyStart = body;
    } else if (header.bodySize != header.storedSize) {
        return false;
    }
    const uint32_t inflated = nowMs();
    const uint8_t* bodyEnd = bodyStart + header.bodySize;

    // Where each section starts, then the stages in draw order
    const uint8_t* sectionData[static_cast<int>(Stage::Count)] = {};
    const uint8_t* sectionEnd[static_cast<int>(Stage::Count)] = {};
    const uint8_t* cursor = bodyStart;
    while (cursor < bodyEnd && *cursor != 0xFF) {
        const uint8_t id = *cursor++;
        uint64_t count;
        uint64_t length;
        if (id >= static_cast<uint8_t>(Stage::Count) || !readVarint(cursor, bodyEnd, count) || !readVarint(cursor, bodyEnd, length) ||
            length > static_cast<uint64_t>(bodyEnd - cursor)) {
            logError("Prepared tile %d/%d/%d is damaged", header.tile.z, header.tile.x, header.tile.y);
            return false;
        }
        sectionData[id] = cursor;
        sectionEnd[id] = cursor + length;
        cursor += length;
    }

    preparedExtent = header.extent;
    begin(header.tile, target, tilePalette, out);
    for (int i = 0; i < static_cast<int>(Stage::Count); i++) {
        const auto drawStage = static_cast<Stage>(i);
        const int section = static_cast<int>(preparedSection(drawStage));
        if (sectionData[section] != nullptr && !playSection(sectionData[section], sectionEnd[section], drawStage)) {
            if (!cancelled) {
                logError("Prepared tile %d/%d/%d is damaged", header.tile.z, header.tile.x, header.tile.y);
            }
            preparedExtent = 0;
            return false;
        }
    }
    preparedExtent = 0;
    finishStats(stats, start, inflated, 1);
    if (stats != nullptr) {
        stats->decompressedBytes = header.bodySize;
    }
    return true;
}

bool TileRenderer::playSection(const uint8_t* data, const uint8_t* end, Stage drawStage) {
    const Layer layer = stageLayer(preparedSection(drawStage));
    layerStages[static_cast<int>(layer)] = drawStage;
    TilePoint chunk[64];
    while (data < end) {
        if (shouldStop()) {
            return false;
        }
        FeatureInfo info;
        info.layer = layer;
        const uint8_t flags = *data++;
        info.geometryType = flags & 3;
        info.maritime = (flags & (1 << 2)) != 0;
        info.ramp = (flags & (1 << 3)) != 0;
        uint64_t value;
        if (!readVarint(data, end, value)) {
            return false;
        }
        info.featureClass = static_cast<Value>(value);
        if ((flags & (1 << 4)) != 0) {
            if (!readVarint(data, end, value)) {
                return false;
            }
            info.subclass = static_cast<Value>(value);
        }
        if ((flags & (1 << 5)) != 0) {
            if (!readVarint(data, end, value)) {
                return false;
            }
            info.brunnel = static_cast<Value>(value);
        }
        if ((flags & (1 << 6)) != 0) {
            if (data >= end) {
                return false;
            }
            info.adminLevel = static_cast<int8_t>(*data++);
        }
        uint64_t rings;
        if (!readVarint(data, end, rings)) {
            return false;
        }
        const bool drawn = beginFeature(info);
        int32_t x = 0;
        int32_t y = 0;
        for (uint64_t ring = 0; ring < rings; ring++) {
            uint64_t ringHeader;
            if (!readVarint(data, end, ringHeader)) {
                return false;
            }
            const uint64_t pointCount = ringHeader >> 1;
            if (drawn) {
                beginRing();
            }
            int count = 0;
            for (uint64_t i = 0; i < pointCount; i++) {
                uint64_t dx;
                uint64_t dy;
                if (!readVarint(data, end, dx) || !readVarint(data, end, dy)) {
                    return false;
                }
                x += unzigzag(dx);
                y += unzigzag(dy);
                if (!drawn) {
                    continue;
                }
                chunk[count++] = { x, y };
                if (count == 64) {
                    points(chunk, count);
                    count = 0;
                }
            }
            if (drawn) {
                points(chunk, count);
                endRing((ringHeader & 1) != 0);
            }
        }
        if (drawn) {
            endFeature();
        }
    }
    return true;
}

bool TileRenderer::beginFeature(const FeatureInfo& info) {
    stage = layerStages[static_cast<int>(info.layer)];
    const int extent = preparedExtent > 0 ? preparedExtent : decoder.extent(info.layer);
    scale = static_cast<float>(TILE_SIZE << dz) / static_cast<float>(extent);
    if (!styleFor(info, stage, zoom, *palette, style)) {
        return false;
    }
    featureCount++;
    featureStartUs = nowUs();
    // At the tile's own z14 buildings are many and small, overzoomed views keep full detail
    cheapFeature = stage == Stage::Building && dz == 0 && zoom == MAX_SOURCE_ZOOM;
    raster.setAntialias(!cheapFeature);
    boundsMinX = 1e30f;
    boundsMinY = 1e30f;
    boundsMaxX = -1e30f;
    boundsMaxY = -1e30f;
    return true;
}

void TileRenderer::beginRing() {
    firstPoint = true;
}

void TileRenderer::addPoint(float x, float y) {
    if (cheapFeature) {
        boundsMinX = std::fmin(boundsMinX, x);
        boundsMinY = std::fmin(boundsMinY, y);
        boundsMaxX = std::fmax(boundsMaxX, x);
        boundsMaxY = std::fmax(boundsMaxY, y);
    }
    if (firstPoint) {
        firstPoint = false;
        hasDirection = false;
        startX = x;
        startY = y;
        if (style.lineWidth >= MIN_JOIN_WIDTH) {
            raster.addDot(x, y, style.lineWidth * 0.5f);
        }
    } else if (style.lineWidth > 0.0f) {
        const float dx = x - lastX;
        const float dy = y - lastY;
        const float length = std::sqrt(dx * dx + dy * dy);
        if (length < 0.05f) {
            return;
        }
        const float half = style.lineWidth * 0.5f;
        raster.addSegment(lastX, lastY, x, y, half);
        // A round join only shows where the line turns noticeably
        const float directionX = dx / length;
        const float directionY = dy / length;
        if (hasDirection && style.lineWidth >= MIN_JOIN_WIDTH && directionX * lastDirectionX + directionY * lastDirectionY < JOIN_COSINE) {
            raster.addDot(lastX, lastY, half);
        }
        hasDirection = true;
        lastDirectionX = directionX;
        lastDirectionY = directionY;
    } else {
        raster.addLine(lastX, lastY, x, y);
    }
    lastX = x;
    lastY = y;
}

void TileRenderer::points(const TilePoint* chunk, int count) {
    for (int i = 0; i < count; i++) {
        addPoint(static_cast<float>(chunk[i].x) * scale - offsetX, static_cast<float>(chunk[i].y) * scale - offsetY);
    }
}

void TileRenderer::endRing(bool closed) {
    if (firstPoint) {
        return;
    }
    if (style.lineWidth > 0.0f) {
        if (closed) {
            raster.addSegment(lastX, lastY, startX, startY, style.lineWidth * 0.5f);
        }
        if (style.lineWidth >= MIN_JOIN_WIDTH) {
            raster.addDot(lastX, lastY, style.lineWidth * 0.5f);
        }
    } else {
        raster.addLine(lastX, lastY, startX, startY);
    }
}

bool TileRenderer::shouldStop() {
    if (cancelFlag != nullptr && cancelFlag->load()) {
        cancelled = true;
    }
    return cancelled;
}

void TileRenderer::endFeature() {
    const uint64_t filled = nowUs();
    geometryUs += filled - featureStartUs;
    if (cheapFeature && boundsMaxX - boundsMinX < MIN_BUILDING_PIXELS && boundsMaxY - boundsMinY < MIN_BUILDING_PIXELS) {
        raster.clear();
    } else {
        raster.fill(pixels, TILE_SIZE, style.color, style.opacity);
    }
    if (cheapFeature) {
        raster.setAntialias(true);
    }
    const uint64_t done = nowUs();
    fillUs += done - filled;
    if (stage == Stage::Building) {
        buildingUs += done - featureStartUs;
    }
}

} // namespace maps
