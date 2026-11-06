// SPDX-License-Identifier: GPL-3.0-or-later
#include "PreparedTile.h"

#include "../platform/Platform.h"
#include "../tiles/TileStore.h"
#include "PreparedLabels.h"

#include <miniz.h>

#include <cmath>
#include <cstring>

namespace maps {

namespace {

constexpr char MAGIC[4] = { 'T', 'M', 'P', 'T' };
constexpr size_t RING_START_CAPACITY = 1024;
/** Longer rings are stored without simplification. */
constexpr size_t RING_MAX_POINTS = 65536;
/** Labels beyond this many bytes are dropped, they are for the screen around the current tile only. */
constexpr size_t LABEL_MAX_BYTES = 48 * 1024;
constexpr uint8_t FLAG_MARITIME = 1 << 2;
constexpr uint8_t FLAG_RAMP = 1 << 3;
constexpr uint8_t FLAG_SUBCLASS = 1 << 4;
constexpr uint8_t FLAG_BRUNNEL = 1 << 5;
constexpr uint8_t FLAG_ADMIN = 1 << 6;

uint16_t readU16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

uint32_t readU32(const uint8_t* bytes) {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) | (static_cast<uint32_t>(bytes[2]) << 16) |
        (static_cast<uint32_t>(bytes[3]) << 24);
}

int32_t roundToInt(float value) {
    return static_cast<int32_t>(std::floor(value + 0.5f));
}

/** The section a layer's features go to, -1 for layers that are not drawn. */
int sectionOf(Layer layer) {
    for (int i = 0; i < static_cast<int>(Stage::Count); i++) {
        const auto stage = static_cast<Stage>(i);
        if (stage == preparedSection(stage) && stageLayer(stage) == layer) {
            return i;
        }
    }
    return -1;
}

/** The label kind of a layer, -1 for layers without labels. */
int labelKindOf(Layer layer) {
    switch (layer) {
        case Layer::Place:
            return static_cast<int>(LabelKind::Place);
        case Layer::TransportationName:
            return static_cast<int>(LabelKind::Road);
        case Layer::WaterName:
            return static_cast<int>(LabelKind::Water);
        case Layer::Waterway:
            return static_cast<int>(LabelKind::Waterway);
        case Layer::MountainPeak:
            return static_cast<int>(LabelKind::Peak);
        default:
            return -1;
    }
}

size_t encodeVarint(uint8_t* out, uint64_t value) {
    size_t count = 0;
    while (value >= 0x80) {
        out[count++] = static_cast<uint8_t>(value | 0x80);
        value >>= 7;
    }
    out[count++] = static_cast<uint8_t>(value);
    return count;
}

int deflateCallback(const void* buffer, int length, void* user) {
    auto* out = static_cast<ByteWriter*>(user);
    out->putBytes(buffer, static_cast<size_t>(length));
    return out->failed() ? 0 : 1;
}

} // namespace

bool readPreparedHeader(const uint8_t* data, size_t size, PreparedHeader& header) {
    if (data == nullptr || size < PREPARED_HEADER_SIZE || memcmp(data, MAGIC, 4) != 0) {
        return false;
    }
    header.version = readU16(data + 4);
    header.flags = readU16(data + 6);
    header.tile.z = data[8];
    header.extent = readU16(data + 10);
    header.tile.x = static_cast<int>(readU32(data + 12));
    header.tile.y = static_cast<int>(readU32(data + 16));
    header.sourceSize = readU32(data + 20);
    header.bodySize = readU32(data + 24);
    header.storedSize = readU32(data + 28);
    memcpy(header.sourceVersion, data + 32, PREPARED_VERSION_LENGTH);
    header.sourceVersion[PREPARED_VERSION_LENGTH] = '\0';
    return true;
}

bool isPreparedCurrent(const PreparedHeader& header, const TileKey& tile, uint32_t sourceSize) {
    return header.version == PREPARED_FORMAT_VERSION && header.tile == tile && header.sourceSize == sourceSize &&
        header.extent == PREPARED_EXTENT;
}

bool isPreparedReadable(const PreparedHeader& header, const TileKey& tile, uint32_t sourceSize) {
    return header.version >= PREPARED_MIN_READABLE_VERSION && header.version <= PREPARED_FORMAT_VERSION &&
        header.tile == tile && header.sourceSize == sourceSize && header.extent == PREPARED_EXTENT;
}

// ---------------------------------------------------------------------------------------------------------------

ByteWriter::~ByteWriter() {
    largeFree(data);
}

void ByteWriter::release() {
    largeFree(data);
    data = nullptr;
    capacity = 0;
    clear();
}

bool ByteWriter::reserveExact(size_t size) {
    if (size <= capacity) {
        return true;
    }
    auto* grown = static_cast<uint8_t*>(largeAlloc(size));
    if (grown == nullptr) {
        error = true;
        return false;
    }
    if (length > 0) {
        memcpy(grown, data, length);
    }
    largeFree(data);
    data = grown;
    capacity = size;
    return true;
}

bool ByteWriter::reserve(size_t size) {
    if (size <= capacity) {
        return true;
    }
    size_t next = capacity < 4096 ? 4096 : capacity;
    while (next < size) {
        next *= 2;
    }
    auto* grown = static_cast<uint8_t*>(largeAlloc(next));
    if (grown == nullptr) {
        error = true;
        return false;
    }
    if (length > 0) {
        memcpy(grown, data, length);
    }
    largeFree(data);
    data = grown;
    capacity = next;
    return true;
}

void ByteWriter::put(uint8_t value) {
    if (length < capacity || reserve(length + 1)) {
        data[length++] = value;
    }
}

void ByteWriter::putBytes(const void* bytes, size_t count) {
    if (count > 0 && reserve(length + count)) {
        memcpy(data + length, bytes, count);
        length += count;
    }
}

void ByteWriter::putVarint(uint64_t value) {
    while (value >= 0x80) {
        put(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    put(static_cast<uint8_t>(value));
}

void ByteWriter::putU16(uint16_t value) {
    put(static_cast<uint8_t>(value));
    put(static_cast<uint8_t>(value >> 8));
}

void ByteWriter::putU32(uint32_t value) {
    for (int i = 0; i < 4; i++) {
        put(static_cast<uint8_t>(value >> (8 * i)));
    }
}

void ByteWriter::setU32(size_t offset, uint32_t value) {
    if (offset + 4 <= length) {
        for (int i = 0; i < 4; i++) {
            data[offset + i] = static_cast<uint8_t>(value >> (8 * i));
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------

void TileConverter::trim(size_t keepBytes) {
    for (auto& writer : sections) {
        if (writer.allocated() > keepBytes) {
            writer.release();
        }
    }
    if (labels.allocated() > keepBytes) {
        labels.release();
    }
}

TileConverter::~TileConverter() {
    largeFree(ringPoints);
    largeFree(keep);
    largeFree(stack);
}

bool TileConverter::growRing(size_t points) {
    if (points <= ringCapacity) {
        return true;
    }
    if (points > RING_MAX_POINTS) {
        return false;
    }
    size_t next = ringCapacity == 0 ? RING_START_CAPACITY : ringCapacity;
    while (next < points) {
        next *= 2;
    }
    auto* grownPoints = static_cast<float*>(largeAlloc(next * 2 * sizeof(float)));
    auto* grownKeep = static_cast<uint8_t*>(largeAlloc(next));
    auto* grownStack = static_cast<uint32_t*>(largeAlloc(next * 2 * sizeof(uint32_t)));
    if (grownPoints == nullptr || grownKeep == nullptr || grownStack == nullptr) {
        largeFree(grownPoints);
        largeFree(grownKeep);
        largeFree(grownStack);
        return false;
    }
    if (ringLength > 0) {
        memcpy(grownPoints, ringPoints, ringLength * 2 * sizeof(float));
    }
    largeFree(ringPoints);
    largeFree(keep);
    largeFree(stack);
    ringPoints = grownPoints;
    keep = grownKeep;
    stack = grownStack;
    ringCapacity = next;
    return true;
}

bool TileConverter::convert(MvtDecoder& tileDecoder, ByteInput* input, const TileKey& sourceTile, uint32_t sourceSize,
    const char* sourceVersion, bool deflate, ByteWriter& out, ConvertStats* stats) {
    const uint32_t start = nowMs();
    decoder = &tileDecoder;
    tile = sourceTile;
    counters = {};
    // A tile is drawn at its own zoom level and magnified for the levels above it while no closer tile exists
    minZoom = tile.z;
    maxZoom = tile.z + TileStore::MAX_OVERZOOM < MAX_VIEW_ZOOM ? tile.z + TileStore::MAX_OVERZOOM : MAX_VIEW_ZOOM;
    // Units per pixel at z16, where the drop and simplification thresholds are measured
    pixelsPerUnitAt16 = tile.z <= 16 ? std::ldexp(static_cast<float>(TILE_SIZE) / PREPARED_EXTENT, 16 - tile.z) : 1.0f;
    for (int i = 0; i < static_cast<int>(Stage::Count); i++) {
        sections[i].clear();
        sectionCounts[i] = 0;
    }
    labels.clear();
    labelCount = 0;
    failureText = "";
    if (!decoder->scan(input)) {
        failureText = "reading the tile";
        return false;
    }
    Layer layers[static_cast<int>(Stage::Count) + 5];
    int layerCount = 0;
    for (int i = 0; i < static_cast<int>(Stage::Count); i++) {
        const auto stage = static_cast<Stage>(i);
        if (stage == preparedSection(stage)) {
            layers[layerCount++] = stageLayer(stage);
        }
    }
    layers[layerCount++] = Layer::Place;
    layers[layerCount++] = Layer::TransportationName;
    layers[layerCount++] = Layer::WaterName;
    layers[layerCount++] = Layer::Waterway;
    layers[layerCount++] = Layer::MountainPeak;
    // The sections are separate buffers, so one pass in file order collects all of them
    if (!decoder->sweep(layers, layerCount, *this)) {
        failureText = "decoding, or no memory for the features";
        return false;
    }
    for (int i = 0; i < static_cast<int>(Stage::Count); i++) {
        if (sections[i].failed()) {
            failureText = "no memory for the features";
            return false;
        }
    }
    if (labels.failed()) {
        failureText = "no memory for the labels";
        return false;
    }

    // The sections go straight into the output: one combined copy of them would double the memory a large tile needs
    size_t bodySize = 1;
    uint8_t head[1 + 2 * 10];
    for (int i = 0; i < static_cast<int>(Stage::Count); i++) {
        if (sectionCounts[i] != 0) {
            bodySize += encodeVarint(head, sectionCounts[i]) + encodeVarint(head, sections[i].size()) + 1 + sections[i].size();
        }
    }
    if (labelCount != 0) {
        bodySize += encodeVarint(head, labelCount) + encodeVarint(head, labels.size()) + 1 + labels.size();
    }

    out.clear();
    out.putBytes(MAGIC, 4);
    out.putU16(PREPARED_FORMAT_VERSION);
    out.putU16(deflate ? PREPARED_FLAG_DEFLATED : 0);
    out.put(static_cast<uint8_t>(tile.z));
    out.put(0);
    out.putU16(PREPARED_EXTENT);
    out.putU32(static_cast<uint32_t>(tile.x));
    out.putU32(static_cast<uint32_t>(tile.y));
    out.putU32(sourceSize);
    out.putU32(static_cast<uint32_t>(bodySize));
    out.putU32(0);
    // Zero padded, without a terminator when the version fills the field
    char version[PREPARED_VERSION_LENGTH] = {};
    if (sourceVersion != nullptr) {
        const size_t length = strlen(sourceVersion);
        memcpy(version, sourceVersion, length < PREPARED_VERSION_LENGTH ? length : PREPARED_VERSION_LENGTH);
    }
    out.putBytes(version, PREPARED_VERSION_LENGTH);
    tdefl_compressor* compressor = nullptr;
    if (deflate) {
        compressor = static_cast<tdefl_compressor*>(largeAlloc(sizeof(tdefl_compressor)));
        // 128 probes per search and a zlib header, so InflateReader recognises the stream
        if (compressor == nullptr || tdefl_init(compressor, deflateCallback, &out, 128 | TDEFL_WRITE_ZLIB_HEADER) != TDEFL_STATUS_OKAY) {
            largeFree(compressor);
            failureText = "no 320 KB block of memory for the compressor";
            return false;
        }
    }
    auto emit = [&](const void* bytes, size_t count) {
        if (compressor == nullptr) {
            out.putBytes(bytes, count);
            return !out.failed();
        }
        return tdefl_compress_buffer(compressor, bytes, count, TDEFL_NO_FLUSH) == TDEFL_STATUS_OKAY;
    };
    bool ok = true;
    for (int i = 0; i < static_cast<int>(Stage::Count) && ok; i++) {
        if (sectionCounts[i] == 0) {
            continue;
        }
        size_t headSize = 0;
        head[headSize++] = static_cast<uint8_t>(i);
        headSize += encodeVarint(head + headSize, sectionCounts[i]);
        headSize += encodeVarint(head + headSize, sections[i].size());
        ok = emit(head, headSize) && emit(sections[i].bytes(), sections[i].size());
    }
    if (ok && labelCount != 0) {
        size_t headSize = 0;
        head[headSize++] = PREPARED_LABEL_SECTION;
        headSize += encodeVarint(head + headSize, labelCount);
        headSize += encodeVarint(head + headSize, labels.size());
        ok = emit(head, headSize) && emit(labels.bytes(), labels.size());
    }
    const uint8_t endMark = 0xFF;
    ok = ok && emit(&endMark, 1);
    if (compressor != nullptr) {
        ok = ok && tdefl_compress_buffer(compressor, nullptr, 0, TDEFL_FINISH) == TDEFL_STATUS_DONE;
        largeFree(compressor);
    }
    if (!ok || out.failed()) {
        failureText = "no memory for the prepared tile";
        return false;
    }
    const auto stored = static_cast<uint32_t>(out.size() - PREPARED_HEADER_SIZE);
    out.setU32(28, stored);
    if (stats != nullptr) {
        *stats = counters;
        stats->totalMs = nowMs() - start;
        stats->bodyBytes = static_cast<uint32_t>(bodySize);
        stats->storedBytes = stored;
    }
    return !out.failed();
}

bool TileConverter::beginFeature(const FeatureInfo& info) {
    counters.featuresIn++;
    section = sectionOf(info.layer);
    const int kind = labelKindOf(info.layer);
    const bool hasLabel = kind >= 0 && info.name != nullptr && info.nameLength > 0;
    // Kept when the style draws it in one of the section's stages at one of the zoom levels the tile is used for
    const Palette& palette = colorPalette();
    bool drawn = false;
    if (section >= 0) {
        for (int zoom = minZoom; zoom <= maxZoom && !drawn; zoom++) {
            for (int i = 0; i < static_cast<int>(Stage::Count) && !drawn; i++) {
                const auto stage = static_cast<Stage>(i);
                DrawStyle style {};
                drawn = static_cast<int>(preparedSection(stage)) == section && styleFor(info, stage, zoom, palette, style);
            }
        }
    }
    if (!drawn && !hasLabel) {
        return false;
    }
    feature = info;
    drawFeature = drawn;
    labelFeature = hasLabel;
    labelKind = static_cast<uint8_t>(kind);
    rings.clear();
    ringCount = 0;
    lastX = 0;
    lastY = 0;
    minX = 1e30f;
    minY = 1e30f;
    maxX = -1e30f;
    maxY = -1e30f;
    unitsPerSourceUnit = static_cast<float>(PREPARED_EXTENT) / static_cast<float>(decoder->extent(info.layer));
    return true;
}

void TileConverter::beginRing() {
    ringLength = 0;
    ringOverflow = false;
}

void TileConverter::points(const TilePoint* chunk, int count) {
    counters.pointsIn += static_cast<uint32_t>(count);
    if (!growRing(ringLength + static_cast<size_t>(count))) {
        // Kept unsimplified in the buffer it has, the rest of a huge ring is lost
        ringOverflow = true;
        count = static_cast<int>(ringCapacity - ringLength);
    }
    for (int i = 0; i < count; i++) {
        const float x = static_cast<float>(chunk[i].x) * unitsPerSourceUnit;
        const float y = static_cast<float>(chunk[i].y) * unitsPerSourceUnit;
        // The decoder repeats the last point of a chunk at the start of the next one
        if (ringLength > 0 && ringPoints[ringLength * 2 - 2] == x && ringPoints[ringLength * 2 - 1] == y) {
            continue;
        }
        ringPoints[ringLength * 2] = x;
        ringPoints[ringLength * 2 + 1] = y;
        ringLength++;
    }
}

int TileConverter::simplifyRing(int count, float tolerance) {
    // Douglas-Peucker with an explicit stack, keeping the end points
    for (int i = 0; i < count; i++) {
        keep[i] = 0;
    }
    keep[0] = 1;
    keep[count - 1] = 1;
    int depth = 0;
    stack[depth++] = 0;
    stack[depth++] = static_cast<uint32_t>(count - 1);
    const float toleranceSquared = tolerance * tolerance;
    while (depth > 0) {
        const auto last = static_cast<int>(stack[--depth]);
        const auto first = static_cast<int>(stack[--depth]);
        if (last - first < 2) {
            continue;
        }
        const float ax = ringPoints[first * 2];
        const float ay = ringPoints[first * 2 + 1];
        const float dx = ringPoints[last * 2] - ax;
        const float dy = ringPoints[last * 2 + 1] - ay;
        const float lengthSquared = dx * dx + dy * dy;
        float worst = -1.0f;
        int worstIndex = -1;
        for (int i = first + 1; i < last; i++) {
            const float px = ringPoints[i * 2] - ax;
            const float py = ringPoints[i * 2 + 1] - ay;
            float distanceSquared;
            if (lengthSquared <= 0.0f) {
                distanceSquared = px * px + py * py;
            } else {
                const float cross = px * dy - py * dx;
                distanceSquared = cross * cross / lengthSquared;
            }
            if (distanceSquared > worst) {
                worst = distanceSquared;
                worstIndex = i;
            }
        }
        if (worst > toleranceSquared) {
            keep[worstIndex] = 1;
            stack[depth++] = static_cast<uint32_t>(first);
            stack[depth++] = static_cast<uint32_t>(worstIndex);
            stack[depth++] = static_cast<uint32_t>(worstIndex);
            stack[depth++] = static_cast<uint32_t>(last);
        }
    }
    int kept = 0;
    for (int i = 0; i < count; i++) {
        if (keep[i] != 0) {
            ringPoints[kept * 2] = ringPoints[i * 2];
            ringPoints[kept * 2 + 1] = ringPoints[i * 2 + 1];
            kept++;
        }
    }
    return kept;
}

void TileConverter::endRing(bool closed) {
    if (ringLength == 0) {
        return;
    }
    // A pixel at z18 (a quarter at z16), about half a unit, so lines stay smooth when magnified up to z19
    const float tolerance = 0.25f / pixelsPerUnitAt16;
    int count = static_cast<int>(ringLength);
    if (count > 2 && !ringOverflow) {
        count = simplifyRing(count, tolerance);
    }
    // Quantised in place, dropping points that land on the previous one
    auto* quantised = reinterpret_cast<int32_t*>(ringPoints);
    int kept = 0;
    for (int i = 0; i < count; i++) {
        const float x = ringPoints[i * 2];
        const float y = ringPoints[i * 2 + 1];
        const int32_t qx = roundToInt(x);
        const int32_t qy = roundToInt(y);
        if (kept > 0 && quantised[kept * 2 - 2] == qx && quantised[kept * 2 - 1] == qy) {
            continue;
        }
        minX = std::fmin(minX, x);
        minY = std::fmin(minY, y);
        maxX = std::fmax(maxX, x);
        maxY = std::fmax(maxY, y);
        quantised[kept * 2] = qx;
        quantised[kept * 2 + 1] = qy;
        kept++;
    }
    if (closed && kept > 1 && quantised[0] == quantised[kept * 2 - 2] && quantised[1] == quantised[kept * 2 - 1]) {
        kept--;
    }
    if (labelFeature) {
        const int minimum = feature.geometryType == GEOMETRY_POINT ? 1 : 2;
        if (kept >= minimum) {
            writeLabel(quantised, kept);
        }
    }
    if (!drawFeature) {
        return;
    }
    if (kept < (closed ? 3 : 2)) {
        return;
    }
    rings.putVarint((static_cast<uint64_t>(kept) << 1) | (closed ? 1 : 0));
    for (int i = 0; i < kept; i++) {
        rings.putZigzag(quantised[i * 2] - lastX);
        rings.putZigzag(quantised[i * 2 + 1] - lastY);
        lastX = quantised[i * 2];
        lastY = quantised[i * 2 + 1];
    }
    counters.pointsKept += static_cast<uint32_t>(kept);
    ringCount++;
}

void TileConverter::writeLabel(const int32_t* points, int count) {
    if (labels.size() >= LABEL_MAX_BYTES) {
        return;
    }
    labels.put(labelKind);
    labels.put(static_cast<uint8_t>(feature.featureClass));
    labels.putVarint(feature.rank >= 0 ? static_cast<uint64_t>(feature.rank) + 1 : 0);
    labels.putVarint(feature.nameLength);
    labels.putBytes(feature.name, feature.nameLength);
    const bool sameLatin = feature.nameLatin != nullptr && feature.nameLatinLength > 0 &&
        feature.nameLatinLength == feature.nameLength && memcmp(feature.name, feature.nameLatin, feature.nameLength) == 0;
    if (feature.nameLatin != nullptr && feature.nameLatinLength > 0 && !sameLatin) {
        labels.putVarint(feature.nameLatinLength);
        labels.putBytes(feature.nameLatin, feature.nameLatinLength);
    } else {
        labels.putVarint(0);
    }
    labels.putVarint(static_cast<uint64_t>(count));
    int32_t x = 0;
    int32_t y = 0;
    for (int i = 0; i < count; i++) {
        labels.putZigzag(points[i * 2] - x);
        labels.putZigzag(points[i * 2 + 1] - y);
        x = points[i * 2];
        y = points[i * 2 + 1];
    }
    labelCount++;
}

void TileConverter::endFeature() {
    // Features smaller than a pixel at z16 are dropped
    if (ringCount == 0 || ((maxX - minX) * pixelsPerUnitAt16 < 1.0f && (maxY - minY) * pixelsPerUnitAt16 < 1.0f)) {
        return;
    }
    ByteWriter& out = sections[section];
    uint8_t flags = feature.geometryType & 3;
    if (feature.maritime) {
        flags |= FLAG_MARITIME;
    }
    if (feature.ramp) {
        flags |= FLAG_RAMP;
    }
    if (feature.subclass != Value::None) {
        flags |= FLAG_SUBCLASS;
    }
    if (feature.brunnel != Value::None) {
        flags |= FLAG_BRUNNEL;
    }
    if (feature.adminLevel >= 0) {
        flags |= FLAG_ADMIN;
    }
    out.put(flags);
    out.putVarint(static_cast<uint64_t>(feature.featureClass));
    if (feature.subclass != Value::None) {
        out.putVarint(static_cast<uint64_t>(feature.subclass));
    }
    if (feature.brunnel != Value::None) {
        out.putVarint(static_cast<uint64_t>(feature.brunnel));
    }
    if (feature.adminLevel >= 0) {
        out.put(static_cast<uint8_t>(feature.adminLevel));
    }
    out.putVarint(ringCount);
    out.putBytes(rings.bytes(), rings.size());
    sectionCounts[section]++;
    counters.featuresKept++;
}

} // namespace maps
