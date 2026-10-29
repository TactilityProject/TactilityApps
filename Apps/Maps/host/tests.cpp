// SPDX-License-Identifier: GPL-3.0-or-later
// Unit tests for the PC build of tiles/, mvt/ and render/. No framework: CHECK counts failures.
// Usage: tests [--update]    --update rewrites the golden hashes and images in the fixture folder.

#include "../main/Source/mvt/ByteInput.h"
#include "../main/Source/mvt/InflateReader.h"
#include "../main/Source/mvt/MvtDecoder.h"
#include "../main/Source/render/CacheIndex.h"
#include "../main/Source/render/CacheWalker.h"
#include "../main/Source/render/Compose.h"
#include "../main/Source/render/PreparedScanner.h"
#include "../main/Source/render/PreparedTile.h"
#include "../main/Source/render/Raster.h"
#include "../main/Source/render/Style.h"
#include "../main/Source/render/TileCache.h"
#include "../main/Source/render/TileCodec.h"
#include "../main/Source/render/TileRenderer.h"
#include "../main/Source/tiles/DownloadPlan.h"
#include "../main/Source/tiles/IoQueue.h"
#include "../main/Source/tiles/RegionMeta.h"
#include "../main/Source/tiles/TileJson.h"
#include "../main/Source/tiles/TileMath.h"
#include "../main/Source/tiles/TilePaths.h"
#include "../main/Source/tiles/TileStore.h"
#include "../main/Source/ui/Canvas.h"
#include "../main/Source/ui/Controls.h"
#include "../main/Source/ui/DownloadFrame.h"
#include "../main/Source/ui/Gestures.h"
#include "../main/Source/ui/Widgets.h"
#include "FakeFont.h"
#include "../main/Source/ui/PositionTracker.h"
#include "../main/Source/ui/Settings.h"
#include "FakePosition.h"
#include "ImageDiff.h"
#include "Png.h"

#include <miniz.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string>
#include <tuple>
#include <vector>

using namespace maps;

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "../tests/fixtures"
#endif

namespace {

int checkCount = 0;
int failureCount = 0;

} // namespace

#define CHECK(condition) \
    do { \
        checkCount++; \
        if (!(condition)) { \
            failureCount++; \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
        } \
    } while (0)

namespace {

using Bytes = std::vector<uint8_t>;

bool nearlyEqual(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

// ---------------------------------------------------------------------------------------------------------------
// Protobuf encoding helpers
// ---------------------------------------------------------------------------------------------------------------

void putVarint(Bytes& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<uint8_t>(value));
}

uint32_t zigzagEncode(int32_t value) {
    return (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
}

void putTag(Bytes& out, uint32_t field, uint32_t wire) {
    putVarint(out, (static_cast<uint64_t>(field) << 3) | wire);
}

void putVarintField(Bytes& out, uint32_t field, uint64_t value) {
    putTag(out, field, 0);
    putVarint(out, value);
}

void putBytesField(Bytes& out, uint32_t field, const Bytes& content) {
    putTag(out, field, 2);
    putVarint(out, content.size());
    out.insert(out.end(), content.begin(), content.end());
}

void putStringField(Bytes& out, uint32_t field, const char* text) {
    Bytes content(text, text + strlen(text));
    putBytesField(out, field, content);
}

/** Geometry commands as a protobuf packed array of command integers and zigzag deltas. */
class GeometryBuilder {
public:
    int32_t cursorX = 0;
    int32_t cursorY = 0;
    Bytes data;

    void moveTo(int32_t x, int32_t y) {
        putVarint(data, (1u << 3) | 1u);
        delta(x, y);
    }

    /** Starts a LineTo command with a point count, then call delta/point for each point. */
    void lineTo(const std::vector<TilePoint>& points) {
        putVarint(data, (static_cast<uint64_t>(points.size()) << 3) | 2u);
        for (const TilePoint& point : points) {
            delta(point.x, point.y);
        }
    }

    void closePath() { putVarint(data, (1u << 3) | 7u); }

private:
    void delta(int32_t x, int32_t y) {
        putVarint(data, zigzagEncode(x - cursorX));
        putVarint(data, zigzagEncode(y - cursorY));
        cursorX = x;
        cursorY = y;
    }
};

Bytes featureMessage(const std::vector<uint32_t>& tags, uint32_t type, const GeometryBuilder& geometry) {
    Bytes feature;
    if (!tags.empty()) {
        Bytes packed;
        for (const uint32_t tag : tags) {
            putVarint(packed, tag);
        }
        putBytesField(feature, 2, packed);
    }
    putVarintField(feature, 3, type);
    putBytesField(feature, 4, geometry.data);
    return feature;
}

Bytes layerMessage(const char* name, const std::vector<Bytes>& features, const std::vector<const char*>& keys, const std::vector<const char*>& values) {
    Bytes layer;
    putStringField(layer, 1, name);
    for (const Bytes& feature : features) {
        putBytesField(layer, 2, feature);
    }
    for (const char* key : keys) {
        putStringField(layer, 3, key);
    }
    for (const char* value : values) {
        Bytes valueMessage;
        putStringField(valueMessage, 1, value);
        putBytesField(layer, 4, valueMessage);
    }
    putVarintField(layer, 5, 4096);
    putVarintField(layer, 15, 2);
    return layer;
}

constexpr int LONG_LINE_POINTS = 600;

TilePoint longLinePoint(int i) {
    return { i * 3 - 100, (i * 7) % 50 - 25 };
}

/** water (polygon), poi (unknown, must be skipped), transportation (two-part line), roads2 (long line). */
Bytes buildTile() {
    Bytes tile;

    GeometryBuilder polygon;
    polygon.moveTo(10, 20);
    polygon.lineTo({ { 30, 20 }, { 30, 40 }, { 10, 40 } });
    polygon.closePath();
    putBytesField(tile, 3, layerMessage("water", { featureMessage({ 0, 0 }, 3, polygon) }, { "class" }, { "lake" }));

    GeometryBuilder point;
    point.moveTo(1, 1);
    putBytesField(tile, 3, layerMessage("poi", { featureMessage({ 0, 0 }, 1, point) }, { "class" }, { "bar" }));

    GeometryBuilder line;
    line.moveTo(5, 5);
    line.lineTo({ { 20, 5 }, { 20, -25 } });
    line.moveTo(100, 100);
    line.lineTo({ { 120, 90 } });
    GeometryBuilder longLine;
    longLine.moveTo(longLinePoint(0).x, longLinePoint(0).y);
    std::vector<TilePoint> rest;
    for (int i = 1; i < LONG_LINE_POINTS; i++) {
        rest.push_back(longLinePoint(i));
    }
    longLine.lineTo(rest);
    putBytesField(
        tile,
        3,
        layerMessage("transportation", { featureMessage({ 0, 0 }, 2, line), featureMessage({ 0, 1 }, 2, longLine) }, { "class" }, { "primary", "minor" })
    );
    return tile;
}

// ---------------------------------------------------------------------------------------------------------------
// Recording sink
// ---------------------------------------------------------------------------------------------------------------

struct RecordedRing {
    std::vector<std::vector<TilePoint>> chunks;
    bool closed = false;

    /** Points with the point that the decoder repeats at the start of each later chunk removed. */
    std::vector<TilePoint> flatten(bool* repeatsOk = nullptr) const {
        std::vector<TilePoint> result;
        for (size_t c = 0; c < chunks.size(); c++) {
            size_t start = 0;
            if (c > 0) {
                start = 1;
                if (repeatsOk != nullptr && !(chunks[c][0].x == chunks[c - 1].back().x && chunks[c][0].y == chunks[c - 1].back().y)) {
                    *repeatsOk = false;
                }
            }
            for (size_t i = start; i < chunks[c].size(); i++) {
                result.push_back(chunks[c][i]);
            }
        }
        return result;
    }
};

struct RecordedFeature {
    FeatureInfo info;
    std::vector<RecordedRing> rings;
    bool ended = false;
};

class RecordingSink final : public GeometrySink {
public:
    std::vector<RecordedFeature> features;
    bool accept = true;
    int balanceErrors = 0;

    bool beginFeature(const FeatureInfo& info) override {
        if (!accept) {
            return false;
        }
        features.push_back({});
        features.back().info = info;
        return true;
    }

    void beginRing() override {
        if (features.empty() || ringOpen) {
            balanceErrors++;
            return;
        }
        ringOpen = true;
        features.back().rings.emplace_back();
    }

    void points(const TilePoint* points, int count) override {
        if (!ringOpen || count <= 0) {
            balanceErrors++;
            return;
        }
        features.back().rings.back().chunks.emplace_back(points, points + count);
    }

    void endRing(bool closed) override {
        if (!ringOpen) {
            balanceErrors++;
            return;
        }
        ringOpen = false;
        features.back().rings.back().closed = closed;
    }

    void endFeature() override {
        if (ringOpen || features.empty()) {
            balanceErrors++;
            return;
        }
        features.back().ended = true;
    }

private:
    bool ringOpen = false;
};

std::string describe(const RecordingSink& sink) {
    std::string text;
    char buffer[128];
    for (const RecordedFeature& feature : sink.features) {
        snprintf(buffer, sizeof(buffer), "F l%d g%d c%d s%d;", static_cast<int>(feature.info.layer), feature.info.geometryType, static_cast<int>(feature.info.featureClass), static_cast<int>(feature.info.subclass));
        text += buffer;
        for (const RecordedRing& ring : feature.rings) {
            text += ring.closed ? "R1:" : "R0:";
            for (const TilePoint& point : ring.flatten()) {
                snprintf(buffer, sizeof(buffer), "%d,%d ", point.x, point.y);
                text += buffer;
            }
            text += ";";
        }
    }
    return text;
}

bool samePoint(const TilePoint& point, int x, int y) {
    return point.x == x && point.y == y;
}

/** Decodes a tile with the given bytes and returns the description of water followed by transportation. */
bool decodeTile(const Bytes& bytes, RecordingSink& water, RecordingSink& roads, InflateReader::Format* format = nullptr) {
    MvtDecoder decoder;
    if (!decoder.init()) {
        return false;
    }
    MemoryInput input(bytes.data(), bytes.size());
    if (!decoder.scan(&input)) {
        return false;
    }
    if (format != nullptr) {
        *format = decoder.format();
    }
    const Layer waterLayer = Layer::Water;
    const Layer roadLayer = Layer::Transportation;
    return decoder.sweep(&waterLayer, 1, water) && decoder.sweep(&roadLayer, 1, roads);
}

// ---------------------------------------------------------------------------------------------------------------
// 1. Tile maths
// ---------------------------------------------------------------------------------------------------------------

void testTileMath() {
    printf("tile maths\n");
    const TileKey budapest = tileAt(47.4979, 19.0402, 14);
    CHECK(budapest == (TileKey { 14, 9058, 5729 }));
    CHECK(tileAt(0.0, 0.0, 1) == (TileKey { 1, 1, 1 }));
    CHECK(tileAt(0.0, 0.0, 0) == (TileKey { 0, 0, 0 }));
    // Edges of the world stay inside the tile grid
    CHECK(tileAt(89.0, 180.0, 3) == (TileKey { 3, 7, 0 }));
    CHECK(tileAt(-89.0, -180.0, 3) == (TileKey { 3, 0, 7 }));

    const int zooms[] = { 0, 10, 19 };
    const double lons[] = { -179.9, -73.5, 0.0, 19.0402, 151.2 };
    const double lats[] = { -80.0, -33.8, 0.0, 47.4979, 84.0 };
    for (const int z : zooms) {
        for (const double lon : lons) {
            CHECK(nearlyEqual(worldXToLon(lonToWorldX(lon, z), z), lon, 1e-9));
        }
        for (const double lat : lats) {
            CHECK(nearlyEqual(worldYToLat(latToWorldY(lat, z), z), lat, 1e-9));
        }
    }

    // Latitudes beyond the Mercator limit are clamped
    for (const int z : zooms) {
        const double top = latToWorldY(MAX_LATITUDE, z);
        const double bottom = latToWorldY(-MAX_LATITUDE, z);
        CHECK(std::isfinite(latToWorldY(89.0, z)));
        CHECK(std::isfinite(latToWorldY(-89.0, z)));
        CHECK(nearlyEqual(latToWorldY(89.0, z), top, 1e-9));
        CHECK(nearlyEqual(latToWorldY(-89.0, z), bottom, 1e-9));
        CHECK(top >= -0.01 && top <= 0.01);
        CHECK(nearlyEqual(bottom, static_cast<double>(TILE_SIZE) * static_cast<double>(1 << z), 0.01));
    }

    const BoundingBox box { 19.00, 47.47, 19.10, 47.53 };
    int64_t sum = 0;
    for (int z = 0; z <= 14; z++) {
        const TileRange range = tileRange(box, z);
        CHECK(range.minX <= range.maxX);
        CHECK(range.minY <= range.maxY);
        sum += range.count();
    }
    CHECK(sum == 58);
    CHECK(tileCount(box, 0, 14) == 58);
    CHECK(tileCount(box, 14, 14) == tileRange(box, 14).count());

    CHECK(ancestorOf({ 14, 9058, 5729 }, 12) == (TileKey { 12, 2264, 1432 }));
    CHECK(ancestorOf({ 14, 9058, 5729 }, 14) == (TileKey { 14, 9058, 5729 }));
    CHECK(ancestorOf({ 14, 9058, 5729 }, 0) == (TileKey { 0, 0, 0 }));
    CHECK(wrapTileX(-1, 3) == 7);
    CHECK(wrapTileX(8, 3) == 0);
    CHECK(wrapTileX(3, 3) == 3);
    CHECK(wrapTileX(-9, 3) == 7);
    CHECK(nearlyEqual(metresPerPixel(0.0, 0), 156543.03, 0.1));
    CHECK(nearlyEqual(metresPerPixel(0.0, 1), 156543.03 / 2.0, 0.1));
    CHECK(metresPerPixel(60.0, 5) < metresPerPixel(0.0, 5));
    CHECK(isValidTile({ 14, 9058, 5729 }));
    CHECK(!isValidTile({ 2, 4, 0 }));
    CHECK(!isValidTile({ 2, 0, -1 }));
}

// ---------------------------------------------------------------------------------------------------------------
// 2. Paths
// ---------------------------------------------------------------------------------------------------------------

void testPaths() {
    printf("paths\n");
    char path[PATH_MAX_LENGTH];
    CHECK(tilePath(path, sizeof(path), "/sd", "budapest", { 14, 9058, 5729 }));
    CHECK(strcmp(path, "/sd/maps/budapest/14/9058/5729.pbf") == 0);
    CHECK(tileFolderPath(path, sizeof(path), "/sd", "budapest", { 14, 9058, 5729 }));
    CHECK(strcmp(path, "/sd/maps/budapest/14/9058") == 0);
    CHECK(cachePath(path, sizeof(path), "/sd", "c3", { 14, 9058, 5729 }));
    CHECK(strcmp(path, "/sd/cache/c3/14/9058/5729") == 0);
    CHECK(cacheFolderPath(path, sizeof(path), "/sd", "m3", { 14, 9058, 5729 }));
    CHECK(strcmp(path, "/sd/cache/m3/14/9058") == 0);
    CHECK(isInsideCache("/sd/cache/c3/14/9058/5729", "/sd"));
    CHECK(isInsideCache("/sd/cache/14", "/sd"));
    CHECK(!isInsideCache("/sd/cache/", "/sd"));
    CHECK(!isInsideCache("/sd/cache", "/sd"));
    CHECK(!isInsideCache("/sd/maps/budapest/14/9058/5729.pbf", "/sd"));
    CHECK(!isInsideCache("/sd/cache/../maps/budapest", "/sd"));
    CHECK(!isInsideCache("/other/cache/c3", "/sd"));
    CHECK(preparedPath(path, sizeof(path), "/sd", "budapest", { 14, 9058, 5729 }));
    CHECK(strcmp(path, "/sd/maps/budapest/14/9058/5729.prt") == 0);
    CHECK(preparedTempPath(path, sizeof(path), "/sd", "budapest", { 14, 9058, 5729 }));
    CHECK(strcmp(path, "/sd/maps/budapest/14/9058/5729.ptm") == 0);
    char small[10];
    CHECK(!tilePath(small, sizeof(small), "/sd", "budapest", { 14, 9058, 5729 }));

    CHECK(isValidRegionName("budapest"));
    CHECK(isValidRegionName("a_b-1"));
    CHECK(!isValidRegionName(""));
    CHECK(!isValidRegionName("toolongname"));
    CHECK(!isValidRegionName("Upper"));
    CHECK(!isValidRegionName("a/b"));
    CHECK(!isValidRegionName("a b"));
    CHECK(!isValidRegionName("a.b"));
}

// ---------------------------------------------------------------------------------------------------------------
// 3. MVT decoding
// ---------------------------------------------------------------------------------------------------------------

void testZigzagHelper() {
    // Sanity of the test encoder itself: known protobuf zigzag values
    CHECK(zigzagEncode(0) == 0);
    CHECK(zigzagEncode(-1) == 1);
    CHECK(zigzagEncode(1) == 2);
    CHECK(zigzagEncode(-20) == 39);
    Bytes encoded;
    putVarint(encoded, 300);
    CHECK(encoded.size() == 2 && encoded[0] == 0xAC && encoded[1] == 0x02);
}

void checkDecodedTile(const RecordingSink& water, const RecordingSink& roads) {
    CHECK(water.balanceErrors == 0);
    CHECK(roads.balanceErrors == 0);

    CHECK(water.features.size() == 1);
    if (water.features.size() == 1) {
        const RecordedFeature& feature = water.features[0];
        CHECK(feature.ended);
        CHECK(feature.info.layer == Layer::Water);
        CHECK(feature.info.geometryType == GEOMETRY_POLYGON);
        CHECK(feature.info.featureClass == Value::lake);
        CHECK(feature.rings.size() == 1);
        if (feature.rings.size() == 1) {
            const RecordedRing& ring = feature.rings[0];
            CHECK(ring.closed);
            const std::vector<TilePoint> points = ring.flatten();
            CHECK(points.size() == 4);
            if (points.size() == 4) {
                CHECK(samePoint(points[0], 10, 20));
                CHECK(samePoint(points[1], 30, 20));
                CHECK(samePoint(points[2], 30, 40));
                CHECK(samePoint(points[3], 10, 40));
            }
        }
    }

    CHECK(roads.features.size() == 2);
    if (roads.features.size() == 2) {
        const RecordedFeature& feature = roads.features[0];
        CHECK(feature.ended);
        CHECK(feature.info.layer == Layer::Transportation);
        CHECK(feature.info.geometryType == GEOMETRY_LINE);
        CHECK(feature.info.featureClass == Value::primary);
        CHECK(feature.rings.size() == 2);
        if (feature.rings.size() == 2) {
            CHECK(!feature.rings[0].closed);
            CHECK(!feature.rings[1].closed);
            const std::vector<TilePoint> first = feature.rings[0].flatten();
            const std::vector<TilePoint> second = feature.rings[1].flatten();
            CHECK(first.size() == 3);
            if (first.size() == 3) {
                CHECK(samePoint(first[0], 5, 5));
                CHECK(samePoint(first[1], 20, 5));
                CHECK(samePoint(first[2], 20, -25));
            }
            CHECK(second.size() == 2);
            if (second.size() == 2) {
                CHECK(samePoint(second[0], 100, 100));
                CHECK(samePoint(second[1], 120, 90));
            }
        }

        // The long line is delivered in chunks of at most CHUNK_POINTS, each later chunk starting with the last
        // point of the one before.
        const RecordedFeature& longFeature = roads.features[1];
        CHECK(longFeature.info.featureClass == Value::minor);
        CHECK(longFeature.rings.size() == 1);
        if (longFeature.rings.size() == 1) {
            const RecordedRing& ring = longFeature.rings[0];
            CHECK(!ring.closed);
            CHECK(ring.chunks.size() > 1);
            bool repeatsOk = true;
            const std::vector<TilePoint> points = ring.flatten(&repeatsOk);
            CHECK(repeatsOk);
            for (const auto& chunk : ring.chunks) {
                CHECK(chunk.size() <= static_cast<size_t>(MvtDecoder::CHUNK_POINTS));
            }
            CHECK(points.size() == static_cast<size_t>(LONG_LINE_POINTS));
            if (points.size() == static_cast<size_t>(LONG_LINE_POINTS)) {
                bool inOrder = true;
                for (int i = 0; i < LONG_LINE_POINTS; i++) {
                    const TilePoint expected = longLinePoint(i);
                    if (!samePoint(points[static_cast<size_t>(i)], expected.x, expected.y)) {
                        inOrder = false;
                    }
                }
                CHECK(inOrder);
            }
        }
    }
}

void testMvtDecoding() {
    printf("mvt decoding\n");
    testZigzagHelper();
    const Bytes tile = buildTile();

    MvtDecoder decoder;
    CHECK(decoder.init());
    MemoryInput input(tile.data(), tile.size());
    CHECK(decoder.scan(&input));
    CHECK(decoder.format() == InflateReader::Format::Raw);
    CHECK(decoder.decompressedSize() == tile.size());
    CHECK(decoder.fileIndex(Layer::Water) == 0);
    CHECK(decoder.fileIndex(Layer::Transportation) == 2);
    CHECK(decoder.fileIndex(Layer::Building) == -1);
    CHECK(decoder.fileIndex(Layer::Aeroway) == -1);
    CHECK(decoder.extent(Layer::Water) == 4096);

    RecordingSink water;
    RecordingSink roads;
    const Layer waterLayer = Layer::Water;
    const Layer roadLayer = Layer::Transportation;
    CHECK(decoder.sweep(&waterLayer, 1, water));
    CHECK(decoder.sweep(&roadLayer, 1, roads));
    checkDecodedTile(water, roads);

    // Both layers in one sweep arrive in file order, a missing layer is ignored, and the poi layer never shows up.
    {
        RecordingSink both;
        const Layer layers[] = { Layer::Transportation, Layer::Building, Layer::Water };
        CHECK(decoder.sweep(layers, 3, both));
        CHECK(both.features.size() == 3);
        if (both.features.size() == 3) {
            CHECK(both.features[0].info.layer == Layer::Water);
            CHECK(both.features[1].info.layer == Layer::Transportation);
            CHECK(both.features[2].info.layer == Layer::Transportation);
        }
        CHECK(both.balanceErrors == 0);
    }

    // A sink that refuses the features gets no geometry and the decoder skips it properly
    {
        RecordingSink refusing;
        refusing.accept = false;
        const Layer layers[] = { Layer::Water, Layer::Transportation };
        CHECK(decoder.sweep(layers, 2, refusing));
        CHECK(refusing.features.empty());
        CHECK(refusing.balanceErrors == 0);
    }

    // The same decoder can scan another tile again
    {
        MemoryInput again(tile.data(), tile.size());
        CHECK(decoder.scan(&again));
        RecordingSink water2;
        CHECK(decoder.sweep(&waterLayer, 1, water2));
        CHECK(water2.features.size() == 1);
    }

    // Exactly CHUNK_POINTS points arrive in one chunk, one more point makes a second chunk with the repeat
    for (const int total : { MvtDecoder::CHUNK_POINTS, MvtDecoder::CHUNK_POINTS + 1, MvtDecoder::CHUNK_POINTS * 2 }) {
        GeometryBuilder geometry;
        geometry.moveTo(0, 0);
        std::vector<TilePoint> rest;
        for (int i = 1; i < total; i++) {
            rest.push_back({ i, -i });
        }
        geometry.lineTo(rest);
        Bytes edgeTile;
        putBytesField(edgeTile, 3, layerMessage("water", { featureMessage({ 0, 0 }, 2, geometry) }, { "class" }, { "lake" }));
        MvtDecoder edgeDecoder;
        CHECK(edgeDecoder.init());
        MemoryInput edgeInput(edgeTile.data(), edgeTile.size());
        CHECK(edgeDecoder.scan(&edgeInput));
        RecordingSink sink;
        CHECK(edgeDecoder.sweep(&waterLayer, 1, sink));
        CHECK(sink.features.size() == 1);
        if (sink.features.size() == 1 && sink.features[0].rings.size() == 1) {
            bool repeatsOk = true;
            const std::vector<TilePoint> points = sink.features[0].rings[0].flatten(&repeatsOk);
            CHECK(repeatsOk);
            CHECK(points.size() == static_cast<size_t>(total));
            bool inOrder = points.size() == static_cast<size_t>(total);
            for (size_t i = 0; inOrder && i < points.size(); i++) {
                inOrder = samePoint(points[i], static_cast<int>(i), -static_cast<int>(i));
            }
            CHECK(inOrder);
        } else {
            CHECK(false);
        }
    }

    // A garbage command id makes the sweep fail instead of looping or crashing
    {
        Bytes geometry;
        putVarint(geometry, (1u << 3) | 5u);
        Bytes feature;
        putVarintField(feature, 3, 2);
        putBytesField(feature, 4, geometry);
        Bytes layer;
        putStringField(layer, 1, "water");
        putBytesField(layer, 2, feature);
        Bytes badTile;
        putBytesField(badTile, 3, layer);
        MvtDecoder badDecoder;
        CHECK(badDecoder.init());
        MemoryInput badInput(badTile.data(), badTile.size());
        CHECK(badDecoder.scan(&badInput));
        RecordingSink sink;
        CHECK(!badDecoder.sweep(&waterLayer, 1, sink));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 4. Inflate
// ---------------------------------------------------------------------------------------------------------------

Bytes zlibCompress(const Bytes& data) {
    mz_ulong size = mz_compressBound(static_cast<mz_ulong>(data.size()));
    Bytes out(size);
    if (mz_compress(out.data(), &size, data.data(), static_cast<mz_ulong>(data.size())) != MZ_OK) {
        return {};
    }
    out.resize(size);
    return out;
}

Bytes gzipCompress(const Bytes& data) {
    size_t bodySize = 0;
    void* body = tdefl_compress_mem_to_heap(data.data(), data.size(), &bodySize, TDEFL_DEFAULT_MAX_PROBES);
    if (body == nullptr) {
        return {};
    }
    Bytes out = { 0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF };
    out.insert(out.end(), static_cast<uint8_t*>(body), static_cast<uint8_t*>(body) + bodySize);
    mz_free(body);
    const uint32_t crc = static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data.data(), data.size()));
    const uint32_t isize = static_cast<uint32_t>(data.size());
    for (int i = 0; i < 4; i++) {
        out.push_back(static_cast<uint8_t>(crc >> (8 * i)));
    }
    for (int i = 0; i < 4; i++) {
        out.push_back(static_cast<uint8_t>(isize >> (8 * i)));
    }
    return out;
}

/** Returns at most a few bytes per read, to test refilling the input buffer. */
class DribbleInput final : public ByteInput {
public:
    DribbleInput(const Bytes& data, size_t step) : data(data), step(step) {}

    int read(uint8_t* buffer, size_t size) override {
        size_t count = data.size() - position;
        if (count > step) {
            count = step;
        }
        if (count > size) {
            count = size;
        }
        memcpy(buffer, data.data() + position, count);
        position += count;
        return static_cast<int>(count);
    }

    bool rewind() override {
        position = 0;
        return true;
    }

private:
    const Bytes& data;
    size_t step;
    size_t position = 0;
};

Bytes readAllByGet(InflateReader& reader) {
    Bytes out;
    while (true) {
        const int byte = reader.get();
        if (byte < 0) {
            break;
        }
        out.push_back(static_cast<uint8_t>(byte));
    }
    return out;
}

/** Compressible data a few times bigger than the 32 KB window. */
Bytes buildLargePayload() {
    Bytes data;
    uint32_t state = 12345;
    for (int i = 0; i < 150000; i++) {
        state = state * 1664525u + 1013904223u;
        const uint32_t r = state >> 16;
        // Mostly repeating text with some noise, so matches reach far back
        data.push_back(static_cast<uint8_t>((i % 211 < 150) ? 'a' + (i % 13) : r & 0xFF));
    }
    return data;
}

void testInflate() {
    printf("inflate\n");
    const Bytes tile = buildTile();
    const Bytes zlibTile = zlibCompress(tile);
    const Bytes gzipTile = gzipCompress(tile);
    CHECK(!zlibTile.empty());
    CHECK(!gzipTile.empty());

    struct Variant {
        const Bytes* bytes;
        InflateReader::Format expected;
        const char* name;
    };
    const Variant variants[] = {
        { &tile, InflateReader::Format::Raw, "raw" },
        { &zlibTile, InflateReader::Format::Zlib, "zlib" },
        { &gzipTile, InflateReader::Format::Gzip, "gzip" },
    };

    for (const Variant& variant : variants) {
        InflateReader reader;
        CHECK(reader.init());
        MemoryInput input(variant.bytes->data(), variant.bytes->size());
        CHECK(reader.begin(&input));
        CHECK(reader.format() == variant.expected);
        const Bytes output = readAllByGet(reader);
        CHECK(output == tile);
        CHECK(!reader.failed());
        CHECK(reader.tell() == tile.size());
        CHECK(reader.get() == -1);

        // begin() again starts over
        MemoryInput again(variant.bytes->data(), variant.bytes->size());
        CHECK(reader.begin(&again));
        Bytes viaRead(tile.size());
        CHECK(reader.read(viaRead.data(), viaRead.size()) == tile.size());
        CHECK(viaRead == tile);

        // skip()
        MemoryInput third(variant.bytes->data(), variant.bytes->size());
        CHECK(reader.begin(&third));
        CHECK(reader.skip(10));
        CHECK(reader.tell() == 10);
        CHECK(reader.get() == tile[10]);
        CHECK(reader.tell() == 11);
        CHECK(reader.skip(tile.size() - 11));
        CHECK(reader.tell() == tile.size());
        CHECK(!reader.skip(1));
    }

    // Decoding the compressed tiles gives the same result as the raw tile
    {
        RecordingSink rawWater;
        RecordingSink rawRoads;
        CHECK(decodeTile(tile, rawWater, rawRoads));
        for (const Variant& variant : variants) {
            RecordingSink water;
            RecordingSink roads;
            InflateReader::Format format = InflateReader::Format::Raw;
            CHECK(decodeTile(*variant.bytes, water, roads, &format));
            CHECK(format == variant.expected);
            CHECK(describe(water) == describe(rawWater));
            CHECK(describe(roads) == describe(rawRoads));
            CHECK(water.features.size() == 1);
            checkDecodedTile(water, roads);
        }
    }

    // A payload larger than the window, read in odd sized pieces and from an input that returns few bytes per read
    {
        const Bytes large = buildLargePayload();
        const Bytes largeZlib = zlibCompress(large);
        const Bytes largeGzip = gzipCompress(large);
        CHECK(largeZlib.size() > 0 && largeZlib.size() < large.size());
        for (const Variant& variant : { Variant { &large, InflateReader::Format::Raw, "raw" }, Variant { &largeZlib, InflateReader::Format::Zlib, "zlib" }, Variant { &largeGzip, InflateReader::Format::Gzip, "gzip" } }) {
            InflateReader reader;
            CHECK(reader.init());
            MemoryInput input(variant.bytes->data(), variant.bytes->size());
            CHECK(reader.begin(&input));
            CHECK(reader.format() == variant.expected);
            Bytes output;
            uint8_t piece[777];
            while (true) {
                const size_t count = reader.read(piece, sizeof(piece));
                output.insert(output.end(), piece, piece + count);
                if (count < sizeof(piece)) {
                    break;
                }
            }
            CHECK(output == large);
            CHECK(!reader.failed());

            DribbleInput dribble(*variant.bytes, 7);
            CHECK(reader.begin(&dribble));
            CHECK(readAllByGet(reader) == large);
            CHECK(!reader.failed());

            MemoryInput third(variant.bytes->data(), variant.bytes->size());
            CHECK(reader.begin(&third));
            CHECK(reader.skip(100000));
            CHECK(reader.get() == large[100000]);
            CHECK(reader.skip(large.size() - 100001));
            CHECK(reader.get() == -1);
        }
    }

    // Truncated and damaged input must not crash and must be reported
    {
        for (const size_t keep : { gzipTile.size() / 2, gzipTile.size() - 9, static_cast<size_t>(12) }) {
            Bytes cut(gzipTile.begin(), gzipTile.begin() + static_cast<std::ptrdiff_t>(keep));
            InflateReader reader;
            CHECK(reader.init());
            MemoryInput input(cut.data(), cut.size());
            const bool started = reader.begin(&input);
            Bytes output = started ? readAllByGet(reader) : Bytes();
            CHECK(!started || reader.failed() || output.size() < tile.size());
            CHECK(output.size() <= tile.size());
        }
        // Header only partly there
        {
            Bytes cut(gzipTile.begin(), gzipTile.begin() + 5);
            InflateReader reader;
            CHECK(reader.init());
            MemoryInput input(cut.data(), cut.size());
            CHECK(!reader.begin(&input) || reader.failed() || readAllByGet(reader).empty());
        }
        // Truncated zlib
        {
            Bytes cut(zlibTile.begin(), zlibTile.begin() + static_cast<std::ptrdiff_t>(zlibTile.size() / 2));
            InflateReader reader;
            CHECK(reader.init());
            MemoryInput input(cut.data(), cut.size());
            CHECK(reader.begin(&input));
            const Bytes output = readAllByGet(reader);
            CHECK(reader.failed() || output.size() < tile.size());
        }
        // The decoder reports a truncated tile
        {
            Bytes cut(gzipTile.begin(), gzipTile.begin() + static_cast<std::ptrdiff_t>(gzipTile.size() / 2));
            MvtDecoder decoder;
            CHECK(decoder.init());
            MemoryInput input(cut.data(), cut.size());
            const bool scanned = decoder.scan(&input);
            RecordingSink sink;
            const Layer layer = Layer::Transportation;
            const bool swept = scanned && decoder.sweep(&layer, 1, sink);
            CHECK(!scanned || !swept || sink.features.size() < 2);
        }
        // Empty input
        {
            InflateReader reader;
            CHECK(reader.init());
            MemoryInput input(nullptr, 0);
            const bool started = reader.begin(&input);
            CHECK(!started || reader.get() == -1);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 5. Raster
// ---------------------------------------------------------------------------------------------------------------

constexpr int RASTER_SIZE = 64;

std::vector<uint16_t> whiteBuffer() {
    return std::vector<uint16_t>(static_cast<size_t>(RASTER_SIZE) * RASTER_SIZE, 0xFFFF);
}

uint16_t pixelAt(const std::vector<uint16_t>& pixels, int x, int y) {
    return pixels[static_cast<size_t>(y) * RASTER_SIZE + static_cast<size_t>(x)];
}

double coverageOf(uint16_t pixel) {
    const int green = (pixel >> 5) & 63;
    return 1.0 - static_cast<double>(green) / 63.0;
}

double totalCoverage(const std::vector<uint16_t>& pixels) {
    double sum = 0.0;
    for (const uint16_t pixel : pixels) {
        sum += coverageOf(pixel);
    }
    return sum;
}

/** True when no pixel of darker is lighter than the same pixel of lighter (black on white, so green tells). */
bool notLighter(const std::vector<uint16_t>& darker, const std::vector<uint16_t>& lighter) {
    for (size_t i = 0; i < darker.size(); i++) {
        if (coverageOf(darker[i]) + 1e-9 < coverageOf(lighter[i])) {
            return false;
        }
    }
    return true;
}

void addRectangle(Raster& raster, float x0, float y0, float x1, float y1, bool reversed) {
    if (!reversed) {
        raster.addLine(x0, y0, x1, y0);
        raster.addLine(x1, y0, x1, y1);
        raster.addLine(x1, y1, x0, y1);
        raster.addLine(x0, y1, x0, y0);
    } else {
        raster.addLine(x0, y0, x0, y1);
        raster.addLine(x0, y1, x1, y1);
        raster.addLine(x1, y1, x1, y0);
        raster.addLine(x1, y0, x0, y0);
    }
}

void testRaster() {
    printf("raster\n");
    Raster raster;
    CHECK(raster.init(RASTER_SIZE, RASTER_SIZE));
    CHECK(raster.width() == RASTER_SIZE);
    CHECK(raster.height() == RASTER_SIZE);
    CHECK(raster.isEmpty());

    // Fractional rectangle
    std::vector<uint16_t> forward = whiteBuffer();
    addRectangle(raster, 2.25f, 2.25f, 12.75f, 7.5f, false);
    CHECK(!raster.isEmpty());
    raster.fill(forward.data(), RASTER_SIZE, 0x0000, 255);
    CHECK(raster.isEmpty());
    bool interiorOk = true;
    for (int y = 3; y <= 6; y++) {
        for (int x = 3; x <= 11; x++) {
            if (pixelAt(forward, x, y) != 0x0000) {
                interiorOk = false;
            }
        }
    }
    CHECK(interiorOk);
    bool outsideOk = true;
    for (int y = 0; y < RASTER_SIZE; y++) {
        for (int x = 0; x < RASTER_SIZE; x++) {
            const bool outside = x < 2 || x > 12 || y < 2 || y > 7;
            if (outside && pixelAt(forward, x, y) != 0xFFFF) {
                outsideOk = false;
            }
        }
    }
    CHECK(outsideOk);
    const double area = 10.5 * 5.25;
    const double covered = totalCoverage(forward);
    CHECK(std::fabs(covered - area) <= area * 0.02);
    if (std::fabs(covered - area) > area * 0.02) {
        printf("  coverage %.3f, expected %.3f\n", covered, area);
    }
    // The edge pixels are partly covered, not all or nothing
    CHECK(pixelAt(forward, 2, 4) != 0xFFFF && pixelAt(forward, 2, 4) != 0x0000);
    CHECK(pixelAt(forward, 12, 4) != 0xFFFF && pixelAt(forward, 12, 4) != 0x0000);

    // Opposite winding gives the same pixels
    std::vector<uint16_t> reversed = whiteBuffer();
    addRectangle(raster, 2.25f, 2.25f, 12.75f, 7.5f, true);
    raster.fill(reversed.data(), RASTER_SIZE, 0x0000, 255);
    CHECK(reversed == forward);

    // Opacity blends instead of replacing
    {
        std::vector<uint16_t> half = whiteBuffer();
        addRectangle(raster, 10.0f, 10.0f, 20.0f, 20.0f, false);
        raster.fill(half.data(), RASTER_SIZE, 0x0000, 128);
        const double c = coverageOf(pixelAt(half, 15, 15));
        CHECK(c > 0.4 && c < 0.6);
        CHECK(pixelAt(half, 25, 25) == 0xFFFF);
    }

    // clear() throws the shape away
    {
        std::vector<uint16_t> cleared = whiteBuffer();
        addRectangle(raster, 10.0f, 10.0f, 20.0f, 20.0f, false);
        raster.clear();
        CHECK(raster.isEmpty());
        raster.fill(cleared.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(cleared == whiteBuffer());
    }

    // Partly outside the raster: left, top, right and bottom
    {
        std::vector<uint16_t> pixels = whiteBuffer();
        addRectangle(raster, -10.0f, -10.0f, 20.0f, 20.0f, false);
        raster.fill(pixels.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(pixelAt(pixels, 0, 0) == 0x0000);
        CHECK(pixelAt(pixels, 5, 5) == 0x0000);
        CHECK(pixelAt(pixels, 19, 19) == 0x0000);
        CHECK(pixelAt(pixels, 20, 20) == 0xFFFF);
        CHECK(pixelAt(pixels, 20, 5) == 0xFFFF);
        CHECK(pixelAt(pixels, 5, 20) == 0xFFFF);
        CHECK(nearlyEqual(totalCoverage(pixels), 400.0, 400.0 * 0.02));

        std::vector<uint16_t> far = whiteBuffer();
        addRectangle(raster, 50.0f, 50.0f, 100.0f, 100.0f, false);
        raster.fill(far.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(pixelAt(far, 49, 49) == 0xFFFF);
        CHECK(pixelAt(far, 50, 50) == 0x0000);
        CHECK(pixelAt(far, 63, 63) == 0x0000);
        CHECK(pixelAt(far, 63, 50) == 0x0000);
        CHECK(pixelAt(far, 50, 63) == 0x0000);
        CHECK(pixelAt(far, 49, 60) == 0xFFFF);
        CHECK(pixelAt(far, 60, 49) == 0xFFFF);
        CHECK(nearlyEqual(totalCoverage(far), 14.0 * 14.0, 14.0 * 14.0 * 0.02));

        // A shape completely outside paints nothing
        std::vector<uint16_t> none = whiteBuffer();
        addRectangle(raster, -50.0f, -50.0f, -10.0f, -10.0f, false);
        addRectangle(raster, 80.0f, 10.0f, 120.0f, 30.0f, false);
        addRectangle(raster, 10.0f, 80.0f, 30.0f, 120.0f, false);
        raster.fill(none.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(none == whiteBuffer());

        // A huge shape, and an edge with huge coordinates
        std::vector<uint16_t> huge = whiteBuffer();
        addRectangle(raster, -1.0e6f, -1.0e6f, 1.0e6f, 1.0e6f, false);
        raster.fill(huge.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(pixelAt(huge, 0, 0) == 0x0000);
        CHECK(pixelAt(huge, 63, 63) == 0x0000);
        CHECK(pixelAt(huge, 32, 32) == 0x0000);
    }

    // A triangle that sticks out on the left side keeps its visible part
    {
        std::vector<uint16_t> pixels = whiteBuffer();
        raster.addLine(-20.0f, 10.0f, 30.0f, 10.0f);
        raster.addLine(30.0f, 10.0f, 30.0f, 40.0f);
        raster.addLine(30.0f, 40.0f, -20.0f, 10.0f);
        raster.fill(pixels.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(pixelAt(pixels, 28, 35) == 0x0000);
        CHECK(pixelAt(pixels, 29, 39) != 0xFFFF);
        CHECK(pixelAt(pixels, 35, 35) == 0xFFFF);
        CHECK(pixelAt(pixels, 2, 11) == 0x0000);
        CHECK(pixelAt(pixels, 2, 25) == 0xFFFF);
        // Area of the visible part: the triangle (-20,10) (30,10) (30,40) clipped at x >= 0
        // Line from (-20,10) to (30,40) has y = 10 + 0.6 * (x + 20); at x=0 y=22, so the visible area is
        // the trapezoid with parallel sides 12 (x=0) and 30 (x=30) over a width of 30 = 630
        CHECK(nearlyEqual(totalCoverage(pixels), 630.0, 630.0 * 0.02));
    }

    // A stroked polyline that doubles back does not cancel
    {
        std::vector<uint16_t> pixels = whiteBuffer();
        raster.addSegment(10.0f, 32.0f, 40.0f, 32.0f, 2.0f);
        raster.addDot(10.0f, 32.0f, 2.0f);
        raster.addDot(40.0f, 32.0f, 2.0f);
        raster.addSegment(40.0f, 32.0f, 10.0f, 32.0f, 2.0f);
        raster.addDot(10.0f, 32.0f, 2.0f);
        raster.fill(pixels.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(pixelAt(pixels, 25, 30) == 0x0000);
        CHECK(pixelAt(pixels, 25, 31) == 0x0000);
        CHECK(pixelAt(pixels, 25, 32) == 0x0000);
        CHECK(pixelAt(pixels, 25, 33) == 0x0000);
        CHECK(pixelAt(pixels, 25, 29) == 0xFFFF);
        CHECK(pixelAt(pixels, 25, 34) == 0xFFFF);
        CHECK(pixelAt(pixels, 40, 32) == 0x0000);
        CHECK(pixelAt(pixels, 10, 32) == 0x0000);
        CHECK(pixelAt(pixels, 41, 32) != 0xFFFF);
        CHECK(pixelAt(pixels, 40, 31) == 0x0000);

        // Drawn once, no pixel is covered more than when doubled back (partly covered edge pixels add up, so they
        // can be darker when doubled, but never lighter)
        std::vector<uint16_t> single = whiteBuffer();
        raster.addSegment(10.0f, 32.0f, 40.0f, 32.0f, 2.0f);
        raster.addDot(10.0f, 32.0f, 2.0f);
        raster.addDot(40.0f, 32.0f, 2.0f);
        raster.fill(single.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(notLighter(pixels, single));

        // Diagonal segments in both directions overlapping each other
        std::vector<uint16_t> diagonal = whiteBuffer();
        raster.addSegment(8.0f, 8.0f, 50.0f, 40.0f, 1.5f);
        raster.addSegment(50.0f, 40.0f, 8.0f, 8.0f, 1.5f);
        raster.addSegment(8.0f, 8.0f, 50.0f, 40.0f, 1.5f);
        raster.fill(diagonal.data(), RASTER_SIZE, 0x0000, 255);
        // The middle of the line at x=29, y=24
        CHECK(pixelAt(diagonal, 29, 24) == 0x0000);
        std::vector<uint16_t> diagonalOnce = whiteBuffer();
        raster.addSegment(8.0f, 8.0f, 50.0f, 40.0f, 1.5f);
        raster.fill(diagonalOnce.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(notLighter(diagonal, diagonalOnce));
    }

    // Holes: opposite winding stays uncovered, equal winding is filled (nonzero rule)
    {
        std::vector<uint16_t> holed = whiteBuffer();
        addRectangle(raster, 10.0f, 10.0f, 50.0f, 50.0f, false);
        addRectangle(raster, 20.0f, 20.0f, 40.0f, 40.0f, true);
        raster.fill(holed.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(pixelAt(holed, 15, 15) == 0x0000);
        CHECK(pixelAt(holed, 45, 45) == 0x0000);
        CHECK(pixelAt(holed, 15, 45) == 0x0000);
        CHECK(pixelAt(holed, 30, 30) == 0xFFFF);
        CHECK(pixelAt(holed, 20, 20) == 0xFFFF);
        CHECK(pixelAt(holed, 39, 39) == 0xFFFF);
        CHECK(pixelAt(holed, 30, 15) == 0x0000);
        CHECK(pixelAt(holed, 30, 45) == 0x0000);
        CHECK(nearlyEqual(totalCoverage(holed), 40.0 * 40.0 - 20.0 * 20.0, 1200.0 * 0.02));

        std::vector<uint16_t> filled = whiteBuffer();
        addRectangle(raster, 10.0f, 10.0f, 50.0f, 50.0f, false);
        addRectangle(raster, 20.0f, 20.0f, 40.0f, 40.0f, false);
        raster.fill(filled.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(pixelAt(filled, 30, 30) == 0x0000);
        CHECK(pixelAt(filled, 15, 15) == 0x0000);

        // Holed shape with the outer ring wound the other way gives the same pixels
        std::vector<uint16_t> holedReversed = whiteBuffer();
        addRectangle(raster, 10.0f, 10.0f, 50.0f, 50.0f, true);
        addRectangle(raster, 20.0f, 20.0f, 40.0f, 40.0f, false);
        raster.fill(holedReversed.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(holedReversed == holed);
    }

    // Degenerate input: NaN, infinity, zero length segments
    {
        std::vector<uint16_t> pixels = whiteBuffer();
        raster.addLine(std::nanf(""), 1.0f, 5.0f, 9.0f);
        raster.addLine(1.0f, 1.0f, INFINITY, 9.0f);
        raster.addSegment(5.0f, 5.0f, 5.0f, 5.0f, 2.0f);
        raster.fill(pixels.data(), RASTER_SIZE, 0x0000, 255);
        CHECK(pixels == whiteBuffer());
    }

    // blendRgb565 end points
    CHECK(blendRgb565(0x0000, 0xFFFF, 0) == 0xFFFF);
    CHECK(blendRgb565(0x0000, 0xFFFF, 32) == 0x0000);
    CHECK(blendRgb565(0xF800, 0x0000, 32) == 0xF800);
}

// ---------------------------------------------------------------------------------------------------------------
// 6. Golden images
// ---------------------------------------------------------------------------------------------------------------

uint64_t fnv1a(const uint16_t* pixels, size_t count) {
    uint64_t hash = 0xCBF29CE484222325ull;
    for (size_t i = 0; i < count; i++) {
        hash ^= static_cast<uint8_t>(pixels[i] & 0xFF);
        hash *= 0x100000001B3ull;
        hash ^= static_cast<uint8_t>(pixels[i] >> 8);
        hash *= 0x100000001B3ull;
    }
    return hash;
}

struct GoldenEntry {
    std::string name;
    uint64_t hash;
};

std::vector<GoldenEntry> readGolden(const char* path, bool* found) {
    std::vector<GoldenEntry> entries;
    FILE* file = fopen(path, "r");
    *found = file != nullptr;
    if (file == nullptr) {
        return entries;
    }
    char name[64];
    unsigned long long hash;
    while (fscanf(file, "%63s %llx", name, &hash) == 2) {
        entries.push_back({ name, static_cast<uint64_t>(hash) });
    }
    fclose(file);
    return entries;
}

void testGolden(bool update) {
    printf("golden images\n");
    const char* goldenPath = FIXTURE_DIR "/golden.txt";
    const char* fixturePath = FIXTURE_DIR "/14-9058-5729.pbf";
    struct Case {
        const char* name;
        TileKey target;
    };
    const Case cases[] = {
        { "tile-14", { 14, 9058, 5729 } },
        { "overzoom-16", { 16, 36232, 22916 } },
        { "overzoom-18", { 18, 144936, 91672 } },
    };
    const TileKey source { 14, 9058, 5729 };

    TileRenderer renderer;
    CHECK(renderer.init());
    std::vector<GoldenEntry> results;
    std::vector<std::vector<uint16_t>> images;
    for (const Case& testCase : cases) {
        FileInput input;
        CHECK(input.open(fixturePath));
        std::vector<uint16_t> pixels(static_cast<size_t>(TILE_SIZE) * TILE_SIZE, 0x1234);
        RenderStats stats {};
        const bool ok = renderer.render(&input, source, testCase.target, colorPalette(), pixels.data(), &stats);
        CHECK(ok);
        CHECK(stats.sweeps <= 4);
        CHECK(stats.features > 100);
        printf("  %s: %u features, %u sweeps, %u bytes inflated\n", testCase.name, stats.features, stats.sweeps, stats.decompressedBytes);
        size_t differing = 0;
        for (const uint16_t pixel : pixels) {
            if (pixel != colorPalette().background) {
                differing++;
            }
        }
        CHECK(differing > pixels.size() / 100);
        results.push_back({ testCase.name, fnv1a(pixels.data(), pixels.size()) });
        images.push_back(std::move(pixels));
        printf("  %s hash %016llx\n", testCase.name, static_cast<unsigned long long>(results.back().hash));
    }

    // Rendering again with the same renderer gives the same pixels
    {
        FileInput input;
        CHECK(input.open(fixturePath));
        std::vector<uint16_t> again(static_cast<size_t>(TILE_SIZE) * TILE_SIZE);
        CHECK(renderer.render(&input, source, cases[0].target, colorPalette(), again.data(), nullptr));
        CHECK(again == images[0]);
    }

    bool found = false;
    const std::vector<GoldenEntry> golden = readGolden(goldenPath, &found);
    if (update || !found || golden.empty()) {
        FILE* file = fopen(goldenPath, "w");
        CHECK(file != nullptr);
        if (file != nullptr) {
            for (const GoldenEntry& entry : results) {
                fprintf(file, "%s %016llx\n", entry.name.c_str(), static_cast<unsigned long long>(entry.hash));
            }
            fclose(file);
            printf("  wrote %s\n", goldenPath);
        }
        CHECK(writePngRgb565(FIXTURE_DIR "/golden-14.png", images[0].data(), TILE_SIZE, TILE_SIZE));
        CHECK(writePngRgb565(FIXTURE_DIR "/golden-16.png", images[1].data(), TILE_SIZE, TILE_SIZE));
        CHECK(writePngRgb565(FIXTURE_DIR "/golden-18.png", images[2].data(), TILE_SIZE, TILE_SIZE));
        return;
    }

    for (size_t i = 0; i < results.size(); i++) {
        const GoldenEntry* expected = nullptr;
        for (const GoldenEntry& entry : golden) {
            if (entry.name == results[i].name) {
                expected = &entry;
            }
        }
        const bool match = expected != nullptr && expected->hash == results[i].hash;
        CHECK(match);
        if (!match) {
            char path[256];
            snprintf(path, sizeof(path), FIXTURE_DIR "/actual-%s.png", results[i].name.c_str());
            writePngRgb565(path, images[i].data(), TILE_SIZE, TILE_SIZE);
            printf("  %s differs: expected %016llx, got %016llx, wrote %s\n", results[i].name.c_str(), expected != nullptr ? static_cast<unsigned long long>(expected->hash) : 0ull, static_cast<unsigned long long>(results[i].hash), path);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 7. Compose
// ---------------------------------------------------------------------------------------------------------------

struct FakeTiles {
    std::vector<uint16_t> pixels[4];
    TileKey skip { -1, -1, -1 };
    bool skipAll = false;
    int calls = 0;
    int lastZoom = -1;
    bool outOfRange = false;

    FakeTiles() {
        const uint16_t colors[4] = { 0xF800, 0x07E0, 0x001F, 0xFFE0 };
        for (int i = 0; i < 4; i++) {
            pixels[i].assign(static_cast<size_t>(TILE_SIZE) * TILE_SIZE, colors[i]);
        }
    }
};

const uint16_t* fakeLookup(const TileKey& tile, void* context) {
    auto* tiles = static_cast<FakeTiles*>(context);
    tiles->calls++;
    tiles->lastZoom = tile.z;
    if (tile.x < 0 || tile.x > 1 || tile.y < 0 || tile.y > 1) {
        tiles->outOfRange = true;
        return nullptr;
    }
    if (tiles->skipAll || tile == tiles->skip) {
        return nullptr;
    }
    return tiles->pixels[tile.y * 2 + tile.x].data();
}

void testCompose() {
    printf("compose\n");
    constexpr uint16_t RED = 0xF800;
    constexpr uint16_t GREEN = 0x07E0;
    constexpr uint16_t BLUE = 0x001F;
    constexpr uint16_t YELLOW = 0xFFE0;
    constexpr uint16_t PLACEHOLDER = 0x8410;
    constexpr int W = 300;
    constexpr int H = 200;

    auto expectedColor = [](int x, int y) -> uint16_t {
        // The view starts at world pixel (106, 156) so the corner of the tiles (256, 256) is at (150, 100)
        if (y < 100) {
            return x < 150 ? RED : GREEN;
        }
        return x < 150 ? BLUE : YELLOW;
    };

    {
        FakeTiles tiles;
        std::vector<uint16_t> view(static_cast<size_t>(W) * H, 0);
        const int missing = composeView(view.data(), W, H, 256.0, 256.0, 1, fakeLookup, &tiles, PLACEHOLDER);
        CHECK(missing == 0);
        CHECK(tiles.calls == 4);
        CHECK(tiles.lastZoom == 1);
        bool allOk = true;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                if (view[static_cast<size_t>(y) * W + static_cast<size_t>(x)] != expectedColor(x, y)) {
                    allOk = false;
                }
            }
        }
        CHECK(allOk);
        CHECK(view[0] == RED);
        CHECK(view[149] == RED);
        CHECK(view[150] == GREEN);
        CHECK(view[static_cast<size_t>(99) * W + 149] == RED);
        CHECK(view[static_cast<size_t>(100) * W + 149] == BLUE);
        CHECK(view[static_cast<size_t>(100) * W + 150] == YELLOW);
        CHECK(view[static_cast<size_t>(H - 1) * W + (W - 1)] == YELLOW);
    }

    // One tile missing
    {
        FakeTiles tiles;
        tiles.skip = { 1, 1, 0 };
        std::vector<uint16_t> view(static_cast<size_t>(W) * H, 0);
        const int missing = composeView(view.data(), W, H, 256.0, 256.0, 1, fakeLookup, &tiles, PLACEHOLDER);
        CHECK(missing == 1);
        bool allOk = true;
        for (int y = 0; y < H; y++) {
            for (int x = 0; x < W; x++) {
                const uint16_t expected = (x >= 150 && y < 100) ? PLACEHOLDER : expectedColor(x, y);
                if (view[static_cast<size_t>(y) * W + static_cast<size_t>(x)] != expected) {
                    allOk = false;
                }
            }
        }
        CHECK(allOk);
    }

    // Every tile missing
    {
        FakeTiles tiles;
        tiles.skipAll = true;
        std::vector<uint16_t> view(static_cast<size_t>(W) * H, 0);
        const int missing = composeView(view.data(), W, H, 256.0, 256.0, 1, fakeLookup, &tiles, PLACEHOLDER);
        CHECK(missing == 4);
        bool allOk = true;
        for (const uint16_t pixel : view) {
            if (pixel != PLACEHOLDER) {
                allOk = false;
            }
        }
        CHECK(allOk);
    }

    // A view across the antimeridian wraps to the tile on the other side (x -1 is tile 1 at zoom 1)
    {
        FakeTiles tiles;
        constexpr int S = 100;
        std::vector<uint16_t> view(static_cast<size_t>(S) * S, 0);
        const int missing = composeView(view.data(), S, S, 0.0, 256.0, 1, fakeLookup, &tiles, PLACEHOLDER);
        CHECK(missing == 0);
        CHECK(!tiles.outOfRange);
        // The view covers world x -50..49, y 206..305
        CHECK(view[static_cast<size_t>(0) * S + 0] == GREEN);
        CHECK(view[static_cast<size_t>(0) * S + 49] == GREEN);
        CHECK(view[static_cast<size_t>(0) * S + 50] == RED);
        CHECK(view[static_cast<size_t>(S - 1) * S + 49] == YELLOW);
        CHECK(view[static_cast<size_t>(S - 1) * S + 50] == BLUE);
    }

    // Above the top of the world there are no tiles, which count as missing without asking for them
    {
        FakeTiles tiles;
        constexpr int S = 100;
        std::vector<uint16_t> view(static_cast<size_t>(S) * S, 0);
        const int missing = composeView(view.data(), S, S, 256.0, 0.0, 1, fakeLookup, &tiles, PLACEHOLDER);
        CHECK(missing == 2);
        CHECK(!tiles.outOfRange);
        CHECK(view[0] == PLACEHOLDER);
        CHECK(view[static_cast<size_t>(S - 1) * S] == RED);
        CHECK(view[static_cast<size_t>(S - 1) * S + (S - 1)] == GREEN);
    }
}

void testTileCodec() {
    printf("tile codec\n");
    // A rendered tile: flat areas, anti-aliased lines and a run longer than one token can hold
    TileRenderer renderer;
    CHECK(renderer.init());
    FileInput input;
    CHECK(input.open(FIXTURE_DIR "/14-9058-5729.pbf"));
    const size_t count = static_cast<size_t>(TILE_SIZE) * TILE_SIZE;
    std::vector<uint16_t> pixels(count);
    CHECK(renderer.render(&input, { 14, 9058, 5729 }, { 14, 9058, 5729 }, colorPalette(), pixels.data(), nullptr));
    std::vector<uint16_t> encoded(count + 256);
    const size_t used = encodeTile(pixels.data(), count, encoded.data(), encoded.size());
    CHECK(used > 0 && used < count);
    printf("  rendered tile: %zu of %zu words (%.0f%%)\n", used, count, 100.0 * used / count);
    std::vector<uint16_t> decoded(count, 0);
    CHECK(maps::decodeTile(encoded.data(), used, decoded.data(), count));
    CHECK(decoded == pixels);

    std::vector<uint16_t> flat(count, 0xABCD);
    const size_t flatUsed = encodeTile(flat.data(), count, encoded.data(), encoded.size());
    CHECK(flatUsed > 0 && flatUsed <= 8);
    CHECK(maps::decodeTile(encoded.data(), flatUsed, decoded.data(), count));
    CHECK(decoded == flat);

    std::vector<uint16_t> noise(count);
    for (size_t i = 0; i < count; i++) {
        noise[i] = static_cast<uint16_t>(i * 2654435761u >> 7);
    }
    const size_t noiseUsed = encodeTile(noise.data(), count, encoded.data(), encoded.size());
    CHECK(noiseUsed > 0 && noiseUsed <= count + 8);
    CHECK(maps::decodeTile(encoded.data(), noiseUsed, decoded.data(), count));
    CHECK(decoded == noise);
    CHECK(encodeTile(noise.data(), count, encoded.data(), count / 2) == 0);
    CHECK(!maps::decodeTile(encoded.data(), 3, decoded.data(), count));
}

// ---------------------------------------------------------------------------------------------------------------
// 8. Download plan
// ---------------------------------------------------------------------------------------------------------------

/** Walks a whole plan with tileAtIndex and checks order, range, uniqueness and the end of the sequence. */
void checkPlanEnumeration(const DownloadPlan& plan) {
    std::set<std::tuple<int, int, int>> seen;
    bool allValid = true;
    bool zoomsOrdered = true;
    bool insideRange = true;
    int previousZoom = plan.minZoom;
    for (int64_t index = 0; index < plan.tiles; index++) {
        TileKey tile { -1, -1, -1 };
        if (!tileAtIndex(plan, index, tile) || !isValidTile(tile)) {
            allValid = false;
            continue;
        }
        if (tile.z < previousZoom) {
            zoomsOrdered = false;
        }
        previousZoom = tile.z;
        if (tile.z < plan.minZoom || tile.z > plan.maxZoom) {
            insideRange = false;
        } else {
            const TileRange range = tileRange(plan.box, tile.z);
            if (tile.x < range.minX || tile.x > range.maxX || tile.y < range.minY || tile.y > range.maxY) {
                insideRange = false;
            }
        }
        seen.insert({ tile.z, tile.x, tile.y });
    }
    CHECK(allValid);
    CHECK(zoomsOrdered);
    CHECK(insideRange);
    CHECK(static_cast<int64_t>(seen.size()) == plan.tiles);
    TileKey tile { -1, -1, -1 };
    CHECK(!tileAtIndex(plan, plan.tiles, tile));
    CHECK(!tileAtIndex(plan, plan.tiles + 1000, tile));
    CHECK(!tileAtIndex(plan, -1, tile));
}

bool withinLimits(int64_t tiles, uint64_t bytes) {
    return tiles <= MAX_DOWNLOAD_TILES && bytes <= MAX_DOWNLOAD_BYTES;
}

void testDownloadPlan() {
    printf("download plan\n");
    CHECK(typicalTileBytes(14) > 0);
    CHECK(typicalTileBytes(20) == typicalTileBytes(14));
    CHECK(typicalTileBytes(0) > 0);
    CHECK(typicalTileBytes(-1) == 0);

    const BoundingBox budapest { 19.03, 47.49, 19.05, 47.505 };
    {
        uint64_t manual = 0;
        for (int z = 0; z <= 14; z++) {
            manual += static_cast<uint64_t>(tileRange(budapest, z).count()) * typicalTileBytes(z);
        }
        CHECK(estimateBytes(budapest, 0, 14) == manual);
        uint64_t partial = 0;
        for (int z = 6; z <= 9; z++) {
            partial += static_cast<uint64_t>(tileRange(budapest, z).count()) * typicalTileBytes(z);
        }
        CHECK(estimateBytes(budapest, 6, 9) == partial);
        CHECK(estimateBytes(budapest, 5, 4) == 0);
    }

    const DownloadPlan small = planDownload(budapest, 14);
    CHECK(small.fits);
    CHECK(small.minZoom == 0);
    CHECK(small.maxZoom == 14);
    CHECK(small.tiles == tileCount(budapest, 0, 14));
    CHECK(small.estimatedBytes == estimateBytes(budapest, 0, 14));
    CHECK(small.estimatedBytes <= MAX_DOWNLOAD_BYTES);
    CHECK(small.tiles > 0 && small.tiles <= MAX_DOWNLOAD_TILES);
    checkPlanEnumeration(small);

    // A lower requested zoom is respected, a higher one is clamped to the source zoom
    {
        const DownloadPlan low = planDownload(budapest, 8);
        CHECK(low.fits);
        CHECK(low.maxZoom == 8);
        CHECK(low.tiles == tileCount(budapest, 0, 8));
        const DownloadPlan high = planDownload(budapest, 19);
        CHECK(high.fits);
        CHECK(high.maxZoom == MAX_SOURCE_ZOOM);
    }

    // A whole country has to lower the max zoom, and the next zoom up would break a limit
    {
        const BoundingBox hungary { 16.1, 45.7, 22.9, 48.6 };
        const DownloadPlan country = planDownload(hungary, 14);
        CHECK(country.fits);
        CHECK(country.maxZoom < 14);
        CHECK(country.maxZoom >= 0);
        CHECK(country.tiles <= MAX_DOWNLOAD_TILES);
        CHECK(country.estimatedBytes <= 50000000ull);
        CHECK(country.tiles == tileCount(hungary, 0, country.maxZoom));
        CHECK(country.estimatedBytes == estimateBytes(hungary, 0, country.maxZoom));
        const int next = country.maxZoom + 1;
        CHECK(!withinLimits(tileCount(hungary, 0, next), estimateBytes(hungary, 0, next)));
        printf("  hungary: max zoom %d, %lld tiles, %llu bytes\n", country.maxZoom, static_cast<long long>(country.tiles), static_cast<unsigned long long>(country.estimatedBytes));
        checkPlanEnumeration(country);
    }

    // The whole world still gives a plan that respects the limits
    {
        const BoundingBox world { -180.0, -85.0, 180.0, 85.0 };
        const DownloadPlan all = planDownload(world, 14);
        CHECK(all.maxZoom <= 14);
        CHECK(all.tiles <= MAX_DOWNLOAD_TILES);
        CHECK(all.estimatedBytes <= MAX_DOWNLOAD_BYTES);
        if (all.fits) {
            CHECK(all.tiles == tileCount(world, 0, all.maxZoom));
            CHECK(all.estimatedBytes == estimateBytes(world, 0, all.maxZoom));
            if (all.maxZoom < 14) {
                const int next = all.maxZoom + 1;
                CHECK(!withinLimits(tileCount(world, 0, next), estimateBytes(world, 0, next)));
            }
            checkPlanEnumeration(all);
        }
        printf("  world: fits %d, max zoom %d, %lld tiles, %llu bytes\n", all.fits ? 1 : 0, all.maxZoom, static_cast<long long>(all.tiles), static_cast<unsigned long long>(all.estimatedBytes));
    }

    // A hand-made plan with a minimum zoom above 0 enumerates from that zoom
    {
        DownloadPlan custom = small;
        custom.minZoom = 10;
        custom.maxZoom = 12;
        custom.tiles = tileCount(custom.box, 10, 12);
        checkPlanEnumeration(custom);
        TileKey first { 0, 0, 0 };
        CHECK(tileAtIndex(custom, 0, first));
        CHECK(first.z == 10);
        const TileRange range = tileRange(custom.box, 10);
        CHECK(first.x == range.minX && first.y == range.minY);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 9. Region meta
// ---------------------------------------------------------------------------------------------------------------

constexpr const char* TEST_TILE_TEMPLATE = "https://tiles.openfreemap.org/planet/20261025_080012_pt/{z}/{x}/{y}.pbf";

bool sameMeta(const RegionMeta& a, const RegionMeta& b) {
    return strcmp(a.name, b.name) == 0 && nearlyEqual(a.box.west, b.box.west, 1e-6) && nearlyEqual(a.box.south, b.box.south, 1e-6) &&
           nearlyEqual(a.box.east, b.box.east, 1e-6) && nearlyEqual(a.box.north, b.box.north, 1e-6) && a.minZoom == b.minZoom &&
           a.maxZoom == b.maxZoom && strcmp(a.source, b.source) == 0 && strcmp(a.version, b.version) == 0 && strcmp(a.date, b.date) == 0 &&
           a.total == b.total && a.done == b.done && a.bytes == b.bytes;
}

void testRegionMeta() {
    printf("region meta\n");
    RegionMeta meta = {};
    strcpy(meta.name, "area01");
    meta.box = { 19.031234, 47.491234, 19.051234, 47.501234 };
    meta.minZoom = 0;
    meta.maxZoom = 13;
    strcpy(meta.source, TEST_TILE_TEMPLATE);
    strcpy(meta.version, "20261025_080012_pt");
    strcpy(meta.date, "2026-10-29");
    meta.total = 1234;
    meta.done = 56;
    meta.bytes = 7890123;

    char text[1024];
    CHECK(formatRegionMeta(meta, text, sizeof(text)));
    RegionMeta parsed = {};
    CHECK(parseRegionMeta(text, parsed));
    CHECK(sameMeta(meta, parsed));
    CHECK(strcmp(parsed.name, "area01") == 0);
    CHECK(parsed.total == 1234 && parsed.done == 56 && parsed.bytes == 7890123);
    CHECK(!parsed.isComplete());

    // Too small an output buffer is refused
    char tiny[40];
    CHECK(!formatRegionMeta(meta, tiny, sizeof(tiny)));

    // The meta.json written by host/fetch_tiles.py (Python json.dump): no name, version, total or bytes
    {
        const char* fetched =
            "{\"bbox\": [16.1, 45.7, 22.9, 48.6], \"minzoom\": 0, \"maxzoom\": 11, "
            "\"source\": \"https://tiles.openfreemap.org/planet/20261025_080012_pt/{z}/{x}/{y}.pbf\", "
            "\"date\": \"2026-10-29\", \"done\": 77}";
        RegionMeta fromScript = {};
        CHECK(parseRegionMeta(fetched, fromScript));
        const BoundingBox hungary { 16.1, 45.7, 22.9, 48.6 };
        CHECK(nearlyEqual(fromScript.box.west, 16.1, 1e-9));
        CHECK(nearlyEqual(fromScript.box.south, 45.7, 1e-9));
        CHECK(nearlyEqual(fromScript.box.east, 22.9, 1e-9));
        CHECK(nearlyEqual(fromScript.box.north, 48.6, 1e-9));
        CHECK(fromScript.minZoom == 0 && fromScript.maxZoom == 11);
        CHECK(strcmp(fromScript.source, TEST_TILE_TEMPLATE) == 0);
        CHECK(strcmp(fromScript.date, "2026-10-29") == 0);
        CHECK(fromScript.total == tileCount(hungary, 0, 11));
        CHECK(fromScript.total > 0);
        CHECK(fromScript.done == 77);
        CHECK(fromScript.name[0] == '\0');
        CHECK(fromScript.bytes == 0);
        CHECK(!fromScript.isComplete());
    }

    // Invalid inputs
    {
        RegionMeta bad = {};
        CHECK(!parseRegionMeta("{\"minzoom\": 0, \"maxzoom\": 5, \"done\": 1}", bad));
        CHECK(!parseRegionMeta("{\"bbox\": [1, 2, 3], \"minzoom\": 0, \"maxzoom\": 5}", bad));
        CHECK(!parseRegionMeta("{\"bbox\": [16.1, 45.7, 22.9, 48.6], \"minzoom\": 0, \"maxzoom\": 15}", bad));
        CHECK(!parseRegionMeta("{\"bbox\": [16.1, 45.7, 22.9, 48.6], \"minzoom\": 6, \"maxzoom\": 5}", bad));
        CHECK(!parseRegionMeta("{\"bbox\": [16.1, 45.7, 22.9, 48.6], \"minzoom\": -1, \"maxzoom\": 5}", bad));
        CHECK(!parseRegionMeta("{\"bbox\": [16.1, 45.7, 22.9, 48.6], \"maxzoom\": 5}", bad));
        CHECK(!parseRegionMeta("this is not json at all", bad));
        CHECK(!parseRegionMeta("", bad));
        // minzoom equal to maxzoom 14 is the valid edge
        CHECK(parseRegionMeta("{\"bbox\": [16.1, 45.7, 22.9, 48.6], \"minzoom\": 14, \"maxzoom\": 14}", bad));
    }

    // Completeness: done >= total > 0
    {
        RegionMeta state = {};
        state.total = 0;
        state.done = 0;
        CHECK(!state.isComplete());
        state.total = 10;
        state.done = 9;
        CHECK(!state.isComplete());
        state.done = 10;
        CHECK(state.isComplete());
        state.done = 11;
        CHECK(state.isComplete());
        state.total = 0;
        state.done = 5;
        CHECK(!state.isComplete());
        const DownloadPlan plan = meta.plan();
        CHECK(plan.minZoom == 0 && plan.maxZoom == 13 && plan.tiles == 1234 && plan.fits);
    }

    // Files: write, read back, overwrite, no ".tmp" left behind
    {
        const std::filesystem::path folder = std::filesystem::temp_directory_path();
        const std::string path = (folder / "tmaps_meta_test.json").string();
        const std::string temporary = path + ".tmp";
        std::filesystem::remove(path);
        std::filesystem::remove(temporary);
        CHECK(path.size() < PATH_MAX_LENGTH);
        CHECK(!readRegionMeta(path.c_str(), parsed));
        CHECK(writeRegionMeta(path.c_str(), meta));
        CHECK(std::filesystem::exists(path));
        CHECK(!std::filesystem::exists(temporary));
        RegionMeta fromFile = {};
        CHECK(readRegionMeta(path.c_str(), fromFile));
        CHECK(sameMeta(meta, fromFile));

        meta.done = 1234;
        meta.bytes = 9999999;
        CHECK(writeRegionMeta(path.c_str(), meta));
        CHECK(!std::filesystem::exists(temporary));
        RegionMeta again = {};
        CHECK(readRegionMeta(path.c_str(), again));
        CHECK(sameMeta(meta, again));
        CHECK(again.isComplete());
        std::filesystem::remove(path);
        std::filesystem::remove(temporary);

        // A folder that does not exist cannot be written
        const std::string missing = (folder / "tmaps_no_such_folder" / "meta.json").string();
        CHECK(!writeRegionMeta(missing.c_str(), meta));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// 10. TileJSON
// ---------------------------------------------------------------------------------------------------------------

void testTileJson() {
    printf("tilejson\n");
    const char* document =
        "{\"tilejson\":\"3.0.0\",\"name\":\"OpenFreeMap Planet\","
        "\"tiles\":[\"https://tiles.openfreemap.org/planet/20261025_080012_pt/{z}/{x}/{y}.pbf\"],"
        "\"minzoom\":0,\"maxzoom\":14,"
        "\"vector_layers\":[{\"id\":\"water\",\"fields\":{\"class\":\"String\"}},{\"id\":\"building\",\"fields\":{}}]}";
    char urlTemplate[160];
    char version[48];
    CHECK(parseTileJson(document, urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
    CHECK(strcmp(urlTemplate, TEST_TILE_TEMPLATE) == 0);
    CHECK(strcmp(version, "20261025_080012_pt") == 0);

    // Pretty printed JSON works as well
    {
        const char* pretty = "{\n  \"tilejson\": \"3.0.0\",\n  \"tiles\": [\n    \"https://tiles.openfreemap.org/planet/20261025_080012_pt/{z}/{x}/{y}.pbf\"\n  ]\n}\n";
        CHECK(parseTileJson(pretty, urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
        CHECK(strcmp(urlTemplate, TEST_TILE_TEMPLATE) == 0);
        CHECK(strcmp(version, "20261025_080012_pt") == 0);
    }

    // Rejected documents
    CHECK(!parseTileJson("{\"tilejson\":\"3.0.0\",\"minzoom\":0}", urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
    CHECK(!parseTileJson("{\"tiles\":[\"http://tiles.openfreemap.org/planet/20261025_080012_pt/{z}/{x}/{y}.pbf\"]}", urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
    CHECK(!parseTileJson("{\"tiles\":[\"https://tiles.openfreemap.org/planet/20261025_080012_pt/{z}/{x}/y.pbf\"]}", urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
    CHECK(!parseTileJson("{\"tiles\":[\"https://tiles.openfreemap.org/planet/20261025_080012_pt/{x}/{y}.pbf\"]}", urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
    CHECK(!parseTileJson("{\"tiles\":[\"https://tile.openstreetmap.org/{z}/{x}/{y}.png\"]}", urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
    CHECK(!parseTileJson("{\"tiles\":[]}", urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
    CHECK(!parseTileJson("{\"tiles\":[\"https://tiles.openfreemap.org/planet/20261025_080012_pt/{z}/{x}/{y}.pbf", urlTemplate, sizeof(urlTemplate), version, sizeof(version)));
    CHECK(!parseTileJson("", urlTemplate, sizeof(urlTemplate), version, sizeof(version)));

    // The template buffer must hold the whole template and its terminator
    {
        const size_t length = strlen(TEST_TILE_TEMPLATE);
        char buffer[160];
        CHECK(!parseTileJson(document, buffer, 20, version, sizeof(version)));
        CHECK(!parseTileJson(document, buffer, length, version, sizeof(version)));
        CHECK(parseTileJson(document, buffer, length + 1, version, sizeof(version)));
        CHECK(strcmp(buffer, TEST_TILE_TEMPLATE) == 0);
    }

    // A version buffer that is too small leaves an empty version instead of overflowing
    {
        char smallVersion[8];
        memset(smallVersion, 'x', sizeof(smallVersion));
        CHECK(parseTileJson(document, urlTemplate, sizeof(urlTemplate), smallVersion, sizeof(smallVersion)));
        CHECK(smallVersion[0] == '\0');
    }

    // Filling in the placeholders
    char url[160];
    CHECK(formatTileUrl(TEST_TILE_TEMPLATE, { 14, 9058, 5729 }, url, sizeof(url)));
    CHECK(strcmp(url, "https://tiles.openfreemap.org/planet/20261025_080012_pt/14/9058/5729.pbf") == 0);
    CHECK(formatTileUrl(TEST_TILE_TEMPLATE, { 0, 0, 0 }, url, sizeof(url)));
    CHECK(strcmp(url, "https://tiles.openfreemap.org/planet/20261025_080012_pt/0/0/0.pbf") == 0);
    {
        const char* expected = "https://tiles.openfreemap.org/planet/20261025_080012_pt/14/9058/5729.pbf";
        const size_t length = strlen(expected);
        char exact[160];
        CHECK(formatTileUrl(TEST_TILE_TEMPLATE, { 14, 9058, 5729 }, exact, length + 1));
        CHECK(strcmp(exact, expected) == 0);
        CHECK(!formatTileUrl(TEST_TILE_TEMPLATE, { 14, 9058, 5729 }, exact, length));
        CHECK(!formatTileUrl(TEST_TILE_TEMPLATE, { 14, 9058, 5729 }, exact, 10));
        CHECK(!formatTileUrl(TEST_TILE_TEMPLATE, { 14, 9058, 5729 }, exact, 1));
    }
    // A buffer that ends inside a placeholder value
    {
        char shortBuffer[sizeof("https://tiles.openfreemap.org/planet/20261025_080012_pt/14/90")];
        CHECK(!formatTileUrl(TEST_TILE_TEMPLATE, { 14, 9058, 5729 }, shortBuffer, sizeof(shortBuffer)));
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Prepared tiles
// ---------------------------------------------------------------------------------------------------------------

std::vector<uint8_t> readFixture(const char* path) {
    std::vector<uint8_t> bytes;
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return bytes;
    }
    uint8_t buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + count);
    }
    fclose(file);
    return bytes;
}

void testPreparedTiles() {
    printf("prepared tiles\n");
    const std::vector<uint8_t> pbf = readFixture(FIXTURE_DIR "/14-9058-5729.pbf");
    CHECK(!pbf.empty());
    const TileKey source { 14, 9058, 5729 };
    TileRenderer renderer;
    CHECK(renderer.init());
    TileConverter converter;
    ByteWriter raw;
    ByteWriter deflated;
    ConvertStats stats {};
    MemoryInput input(pbf.data(), pbf.size());
    CHECK(converter.convert(renderer.tileDecoder(), &input, source, static_cast<uint32_t>(pbf.size()), "20261025_080012_pt", false, raw, &stats));
    CHECK(converter.convert(renderer.tileDecoder(), &input, source, static_cast<uint32_t>(pbf.size()), "20261025_080012_pt", true, deflated, nullptr));
    printf("  pbf %zu bytes, raw %zu, deflated %zu, features %u -> %u\n", pbf.size(), raw.size(), deflated.size(), stats.featuresIn, stats.featuresKept);
    CHECK(raw.size() < pbf.size() / 2);
    CHECK(deflated.size() < raw.size());
    CHECK(stats.featuresKept > 100 && stats.featuresKept <= stats.featuresIn);

    // Header
    PreparedHeader header {};
    CHECK(readPreparedHeader(raw.bytes(), raw.size(), header));
    CHECK(header.version == PREPARED_FORMAT_VERSION);
    CHECK(header.tile == source);
    CHECK(header.extent == PREPARED_EXTENT);
    CHECK(header.sourceSize == pbf.size());
    CHECK(header.flags == 0);
    CHECK(header.bodySize == header.storedSize);
    CHECK(PREPARED_HEADER_SIZE + header.storedSize == raw.size());
    CHECK(strcmp(header.sourceVersion, "20261025_080012_pt") == 0);
    CHECK(isPreparedCurrent(header, source, static_cast<uint32_t>(pbf.size())));
    CHECK(!isPreparedCurrent(header, source, static_cast<uint32_t>(pbf.size()) + 1));
    CHECK(!isPreparedCurrent(header, { 14, 9058, 5730 }, static_cast<uint32_t>(pbf.size())));
    PreparedHeader older = header;
    older.version = PREPARED_FORMAT_VERSION - 1;
    CHECK(!isPreparedCurrent(older, source, static_cast<uint32_t>(pbf.size())));
    PreparedHeader deflatedHeader {};
    CHECK(readPreparedHeader(deflated.bytes(), deflated.size(), deflatedHeader));
    CHECK((deflatedHeader.flags & PREPARED_FLAG_DEFLATED) != 0);
    CHECK(deflatedHeader.bodySize == header.bodySize);
    CHECK(!readPreparedHeader(pbf.data(), pbf.size(), header));

    // The prepared copy renders like the original, within a small tolerance, at the native and overzoomed levels.
    // z18 is the golden-18 tile: with 1024 units per tile its lines were visibly stepped (3.0 % of pixels strongly off, mean 3.2).
    const TileKey targets[] = { source, { 16, 36232, 22916 }, { 18, 144936, 91672 } };
    const double maxMean[] = { 3.0, 4.0, 3.0 };
    const double maxStrong[] = { 0.005, 0.02, 0.006 };
    const char* failedNames[] = { FIXTURE_DIR "/actual-prt-14.png", FIXTURE_DIR "/actual-prt-16.png", FIXTURE_DIR "/actual-prt-18.png" };
    for (int i = 0; i < 3; i++) {
        std::vector<uint16_t> original(static_cast<size_t>(TILE_SIZE) * TILE_SIZE);
        std::vector<uint16_t> fromRaw(original.size());
        std::vector<uint16_t> fromDeflated(original.size());
        MemoryInput again(pbf.data(), pbf.size());
        CHECK(renderer.render(&again, source, targets[i], colorPalette(), original.data(), nullptr));
        RenderStats preparedStats {};
        CHECK(renderer.renderPrepared(raw.bytes(), raw.size(), targets[i], colorPalette(), fromRaw.data(), &preparedStats));
        CHECK(renderer.renderPrepared(deflated.bytes(), deflated.size(), targets[i], colorPalette(), fromDeflated.data(), nullptr));
        CHECK(fromRaw == fromDeflated);
        const ImageDiff diff = compareImages(original.data(), fromRaw.data(), original.size());
        printf("  z%d: mean difference %.2f, strong %.2f%%, %u features drawn\n", targets[i].z, diff.meanDifference, 100.0 * diff.strongShare, preparedStats.features);
        CHECK(diff.meanDifference < maxMean[i]);
        CHECK(diff.strongShare < maxStrong[i]);
        if (diff.meanDifference >= maxMean[i] || diff.strongShare >= maxStrong[i]) {
            writePngRgb565(failedNames[i], fromRaw.data(), TILE_SIZE, TILE_SIZE);
        }
    }

    // A prepared tile only draws its own tile and the tiles inside it
    std::vector<uint16_t> pixels(static_cast<size_t>(TILE_SIZE) * TILE_SIZE);
    CHECK(!renderer.renderPrepared(raw.bytes(), raw.size(), { 14, 9059, 5729 }, colorPalette(), pixels.data(), nullptr));
    CHECK(!renderer.renderPrepared(raw.bytes(), raw.size(), { 13, 4529, 2864 }, colorPalette(), pixels.data(), nullptr));
    // Damaged files are refused, not drawn
    CHECK(!renderer.renderPrepared(raw.bytes(), raw.size() / 2, source, colorPalette(), pixels.data(), nullptr));
    std::vector<uint8_t> broken(deflated.bytes(), deflated.bytes() + deflated.size());
    broken[PREPARED_HEADER_SIZE + 20] ^= 0x5A;
    CHECK(!renderer.renderPrepared(broken.data(), broken.size(), source, colorPalette(), pixels.data(), nullptr));
    std::vector<uint8_t> badSection(raw.bytes(), raw.bytes() + raw.size());
    badSection[PREPARED_HEADER_SIZE] = 0x7F;
    CHECK(!renderer.renderPrepared(badSection.data(), badSection.size(), source, colorPalette(), pixels.data(), nullptr));

    // A prepared tile of a vector tile without drawn layers is valid and draws the background
    {
        Bytes layer;
        putVarintField(layer, 15, 2);
        putStringField(layer, 1, "poi");
        Bytes tile;
        putBytesField(tile, 3, layer);
        MemoryInput emptyInput(tile.data(), tile.size());
        ByteWriter out;
        CHECK(converter.convert(renderer.tileDecoder(), &emptyInput, { 3, 1, 2 }, 0, nullptr, false, out, nullptr));
        CHECK(renderer.renderPrepared(out.bytes(), out.size(), { 3, 1, 2 }, colorPalette(), pixels.data(), nullptr));
        bool plain = true;
        for (const uint16_t pixel : pixels) {
            plain = plain && pixel == colorPalette().background;
        }
        CHECK(plain);
    }

    // ByteWriter basics
    ByteWriter writer;
    writer.putVarint(300);
    writer.putZigzag(-3);
    writer.putU32(0x01020304);
    CHECK(writer.size() == 7);
    CHECK(writer.bytes()[0] == 0xAC && writer.bytes()[1] == 0x02);
    CHECK(writer.bytes()[2] == 5);
    CHECK(writer.bytes()[3] == 4 && writer.bytes()[6] == 1);
    writer.setU32(3, 0xAABBCCDD);
    CHECK(writer.bytes()[3] == 0xDD);
    for (int i = 0; i < 100000; i++) {
        writer.put(static_cast<uint8_t>(i));
    }
    CHECK(writer.size() == 100007 && !writer.failed());
    CHECK(writer.bytes()[100006] == static_cast<uint8_t>(99999));
}

// ---------------------------------------------------------------------------------------------------------------
// Prepared tiles on the card, settings
// ---------------------------------------------------------------------------------------------------------------

bool writeBytes(const std::filesystem::path& path, const uint8_t* bytes, size_t size) {
    std::filesystem::create_directories(path.parent_path());
    FILE* file = fopen(path.string().c_str(), "wb");
    if (file == nullptr) {
        return false;
    }
    const bool ok = fwrite(bytes, 1, size, file) == size;
    fclose(file);
    return ok;
}

struct Collected {
    std::vector<std::pair<TileKey, int>> tiles;
    size_t limit = 1000;
};

bool collectStale(const TileKey& tile, int region, void* context) {
    auto* collected = static_cast<Collected*>(context);
    if (collected->tiles.size() >= collected->limit) {
        return false;
    }
    collected->tiles.push_back({ tile, region });
    return true;
}

void testPreparedScanner() {
    printf("prepared scanner\n");
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "tactilitymaps-tests";
    std::error_code error;
    fs::remove_all(root, error);
    const std::vector<uint8_t> pbf = readFixture(FIXTURE_DIR "/14-9058-5729.pbf");
    const TileKey tiles[] = { { 14, 9058, 5729 }, { 14, 9058, 5730 }, { 14, 9059, 5729 }, { 13, 4529, 2864 } };
    for (const TileKey& tile : tiles) {
        char path[PATH_MAX_LENGTH];
        CHECK(tilePath(path, sizeof(path), root.string().c_str(), "city", tile));
        CHECK(writeBytes(path, pbf.data(), pbf.size()));
    }
    // A stray file and a folder that is not a number are skipped
    CHECK(writeBytes(root / "maps" / "city" / "14" / "9058" / "notes.txt", pbf.data(), 4));
    fs::create_directories(root / "maps" / "city" / "14" / "tmp", error);
    RegionMeta meta {};
    strcpy(meta.version, "20261025_080012_pt");
    meta.total = 1;
    meta.done = 1;
    CHECK(writeRegionMeta((root / "maps" / "city" / "meta.json").string().c_str(), meta));

    TileStore store;
    store.setRoot(root.string().c_str());
    CHECK(store.scanRegions() == 1);
    CHECK(strcmp(store.regionVersion(0), "20261025_080012_pt") == 0);
    TileFile file {};
    CHECK(store.find({ 16, 36232, 22916 }, file));
    CHECK(file.tile == tiles[0]);
    CHECK(file.size == pbf.size());
    CHECK(file.region == 0);

    // Lookups skip regions that cannot have a tile, each look costs a FAT folder search on the device
    CHECK(store.mayHave(0, tiles[0]));
    store.setRegionBounds("city", { 19.03, 47.49, 19.05, 47.50 }, 0, 14);
    CHECK(store.mayHave(0, tiles[0]));
    CHECK(store.mayHave(0, { 14, 9059, 5729 }));
    CHECK(!store.mayHave(0, { 14, 9200, 5729 }));
    CHECK(!store.mayHave(0, { 14, 9058, 5000 }));
    store.setRegionBounds("city", { 19.03, 47.49, 19.05, 47.50 }, 0, 12);
    CHECK(!store.mayHave(0, tiles[0]));
    CHECK(!store.find({ 16, 36232, 22916 }, file) || file.tile.z <= 12);
    // A zoom range with min above max covers all zoom levels
    store.setRegionBounds("city", { 19.03, 47.49, 19.05, 47.50 }, 15, 14);
    CHECK(store.mayHave(0, tiles[0]));
    CHECK(store.mayHave(0, { 3, 4, 2 }));
    CHECK(store.find({ 16, 36232, 22916 }, file) && file.tile == tiles[0]);

    PreparedScanner scanner;
    Collected collected;
    int steps = 0;
    while (scanner.step(store, collectStale, &collected) && steps < 100) {
        steps++;
    }
    CHECK(scanner.isComplete());
    CHECK(collected.tiles.size() == 4);

    // Converting one makes it current, the scan then skips it
    TileRenderer renderer;
    CHECK(renderer.init());
    TileConverter converter;
    ByteWriter out;
    MemoryInput input(pbf.data(), pbf.size());
    CHECK(converter.convert(renderer.tileDecoder(), &input, tiles[0], static_cast<uint32_t>(pbf.size()), store.regionVersion(0), true, out, nullptr));
    char prepared[PATH_MAX_LENGTH];
    CHECK(preparedPath(prepared, sizeof(prepared), root.string().c_str(), "city", tiles[0]));
    CHECK(std::string(prepared).substr(std::string(prepared).size() - 13) == "9058/5729.prt");
    CHECK(writeBytes(prepared, out.bytes(), out.size()));
    CHECK(PreparedScanner::isCurrent(prepared, tiles[0], static_cast<uint32_t>(pbf.size())));
    CHECK(!PreparedScanner::isCurrent(prepared, tiles[0], static_cast<uint32_t>(pbf.size()) - 1));
    CHECK(!PreparedScanner::isCurrent(prepared, tiles[1], static_cast<uint32_t>(pbf.size())));
    // A prepared tile with the wrong source tile or a broken header counts as stale
    char second[PATH_MAX_LENGTH];
    CHECK(preparedPath(second, sizeof(second), root.string().c_str(), "city", tiles[1]));
    CHECK(writeBytes(second, out.bytes(), out.size()));
    CHECK(!PreparedScanner::isCurrent(second, tiles[1], static_cast<uint32_t>(pbf.size())));
    scanner.restart();
    collected.tiles.clear();
    while (scanner.step(store, collectStale, &collected)) {
    }
    CHECK(collected.tiles.size() == 3);
    bool hasFirst = false;
    for (const auto& entry : collected.tiles) {
        hasFirst = hasFirst || entry.first == tiles[0];
    }
    CHECK(!hasFirst);

    // A full queue stops a step, the same folder is looked at again
    scanner.restart();
    collected.tiles.clear();
    collected.limit = 1;
    int guard = 0;
    while (scanner.step(store, collectStale, &collected) && guard++ < 200) {
        if (collected.tiles.size() == 1 && collected.limit == 1) {
            collected.limit = 1000;
        }
    }
    CHECK(collected.tiles.size() == 3);

    // The original is never touched
    char original[PATH_MAX_LENGTH];
    CHECK(tilePath(original, sizeof(original), root.string().c_str(), "city", tiles[0]));
    CHECK(readFixture(original) == pbf);
    fs::remove_all(root, error);
}

void testSettings() {
    printf("settings\n");
    Settings settings;
    CHECK(!settings.sdCache && settings.deflatePrepared && settings.smoothPinch && settings.browseDownload);
    parseSettings("sd_cache=0\nunknown=1\ndeflate_prepared=x\nsmooth_pinch=0", settings);
    CHECK(!settings.sdCache);
    CHECK(settings.deflatePrepared);
    CHECK(!settings.smoothPinch);
    CHECK(!settings.devMonochrome && !settings.devProbe);
    parseSettings("dev_mono=1", settings);
    CHECK(settings.devMonochrome);
    settings.devMonochrome = false;
    char text[128];
    CHECK(formatSettings(settings, text, sizeof(text)));
    CHECK(strcmp(text, "sd_cache=0\ndeflate_prepared=1\nsmooth_pinch=0\nbrowse_download=1\ndev_mono=0\ndev_probe=0\n") == 0);
    Settings parsed;
    parseSettings(text, parsed);
    CHECK(!parsed.sdCache && parsed.deflatePrepared && !parsed.smoothPinch);
    CHECK(!formatSettings(settings, text, 10));
    const std::string path = (std::filesystem::temp_directory_path() / "tactilitymaps-settings.txt").string();
    CHECK(writeSettings(path.c_str(), settings));
    Settings read;
    CHECK(readSettings(path.c_str(), read));
    CHECK(!read.sdCache && !read.smoothPinch);
    std::remove(path.c_str());
    Settings missing;
    CHECK(!readSettings(path.c_str(), missing));
    CHECK(!missing.sdCache);

    // The last view
    SavedView view;
    view.lat = 52.2186;
    view.lon = -4.4206;
    view.zoom = 16;
    CHECK(formatSavedView(view, text, sizeof(text)));
    CHECK(strcmp(text, "52218600 -4420600 16\n") == 0);
    SavedView back;
    CHECK(parseSavedView(text, back));
    CHECK(nearlyEqual(back.lat, 52.2186, 1e-9) && nearlyEqual(back.lon, -4.4206, 1e-9) && back.zoom == 16);
    CHECK(!parseSavedView("", back));
    CHECK(!parseSavedView("52218600 4420600", back));
    CHECK(!parseSavedView("90000000 0 14", back));
    CHECK(!parseSavedView("0 0 20", back));
    CHECK(!parseSavedView("x 0 14", back));
    const std::string viewPath = (std::filesystem::temp_directory_path() / "tactilitymaps-view.txt").string();
    CHECK(writeSavedView(viewPath.c_str(), view));
    SavedView readView;
    CHECK(readSavedView(viewPath.c_str(), readView));
    CHECK(readView.zoom == 16 && nearlyEqual(readView.lat, 52.2186, 1e-9));
    std::remove(viewPath.c_str());
    CHECK(!readSavedView(viewPath.c_str(), readView));
}

// ---------------------------------------------------------------------------------------------------------------
// Tile cache: magnified tiles go stale but stay visible until redrawn
// ---------------------------------------------------------------------------------------------------------------

void testTileCache() {
    printf("tile cache\n");
    TileCache cache;
    CHECK(cache.init(3) == 3);
    // Drawn from its z12 parent because its own z14 tile was not on the card yet
    const TileKey magnified { 14, 100, 200 };
    const TileKey native { 14, 25, 50 };
    uint16_t* pixels = cache.reserve(magnified);
    CHECK(pixels != nullptr);
    pixels[0] = 0x1111;
    cache.commit(magnified, 12);
    pixels = cache.reserve(native);
    CHECK(pixels != nullptr);
    cache.commit(native, 14);
    CHECK(cache.contains(magnified) && cache.contains(native));

    // After a download stored tiles, the magnified tile must be drawn again, but is still shown meanwhile
    cache.dropMagnified();
    CHECK(!cache.contains(magnified));
    CHECK(cache.contains(native));
    CHECK(cache.find(magnified) != nullptr && cache.find(magnified)[0] == 0x1111);

    // Its replacement takes over on commit, and only one copy is left
    pixels = cache.reserve(magnified);
    CHECK(pixels != nullptr);
    CHECK(cache.find(magnified) != nullptr && cache.find(magnified)[0] == 0x1111);
    pixels[0] = 0x2222;
    cache.commit(magnified, 14);
    CHECK(cache.contains(magnified));
    CHECK(cache.find(magnified)[0] == 0x2222);
    cache.dropMagnified();
    CHECK(cache.contains(magnified));
    // Both remaining slots are in use by current tiles: the old copy was freed for reuse
    const TileKey other { 14, 26, 50 };
    CHECK(cache.reserve(other) != nullptr);
    cache.release(other);
    CHECK(cache.contains(native));
}

// ---------------------------------------------------------------------------------------------------------------
// I/O queue order
// ---------------------------------------------------------------------------------------------------------------

void testIoQueue() {
    printf("io queue\n");
    IoQueue<int, 8> queue;
    int item = -1;
    CHECK(!queue.pop(item));
    CHECK(queue.push(1, IoPriority::Background));
    CHECK(queue.push(2, IoPriority::Prefetch));
    CHECK(queue.push(3, IoPriority::Background));
    CHECK(queue.push(4, IoPriority::Visible));
    CHECK(queue.push(5, IoPriority::Prefetch));
    CHECK(queue.push(6, IoPriority::Visible));
    CHECK(queue.hasMoreUrgent(IoPriority::Prefetch));
    CHECK(!queue.hasMoreUrgent(IoPriority::Visible));
    // Visible first, then prefetch, then background, the oldest first within each
    const int expected[] = { 4, 6, 2, 5, 1, 3 };
    const IoPriority priorities[] = { IoPriority::Visible, IoPriority::Visible, IoPriority::Prefetch, IoPriority::Prefetch,
        IoPriority::Background, IoPriority::Background };
    for (int i = 0; i < 6; i++) {
        IoPriority priority = IoPriority::Count;
        CHECK(queue.pop(item, &priority));
        CHECK(item == expected[i]);
        CHECK(priority == priorities[i]);
        if (i == 1) {
            // A visible job arriving later still goes before the waiting prefetch
            CHECK(queue.push(7, IoPriority::Visible));
            CHECK(queue.pop(item, &priority));
            CHECK(item == 7);
        }
    }
    CHECK(queue.size() == 0);
    CHECK(!queue.hasMoreUrgent(IoPriority::Background));
    for (int i = 0; i < 8; i++) {
        CHECK(queue.push(i, IoPriority::Background));
    }
    CHECK(!queue.push(9, IoPriority::Visible));
}

// ---------------------------------------------------------------------------------------------------------------
// SD card cache: index, cap and tidying
// ---------------------------------------------------------------------------------------------------------------

void testCacheIndex() {
    printf("cache index\n");
    CacheIndex index;
    CHECK(index.init(10, 1000));
    TileKey tile {};
    CHECK(!index.nextEviction(tile));
    index.put({ 14, 1, 1 }, 300);
    index.put({ 14, 1, 2 }, 300);
    index.put({ 14, 1, 3 }, 300);
    CHECK(index.totalBytes() == 900);
    CHECK(index.contains({ 14, 1, 2 }));
    CHECK(!index.nextEviction(tile));
    // Reading tile 1 makes tile 2 the least recently used
    index.touch({ 14, 1, 1 });
    index.put({ 14, 1, 4 }, 300);
    CHECK(index.isOverCap());
    CHECK(index.nextEviction(tile));
    CHECK(tile == TileKey({ 14, 1, 2 }));
    index.erase(tile);
    CHECK(index.totalBytes() == 900);
    // Eviction stops at 90 % of the cap
    CHECK(!index.nextEviction(tile));
    // Replacing a tile updates the total
    index.put({ 14, 1, 1 }, 100);
    CHECK(index.totalBytes() == 700);
    CHECK(index.size() == 3);

    // Saved and loaded with sizes and order of use
    ByteWriter bytes;
    index.save(bytes);
    CacheIndex loaded;
    CHECK(loaded.init(10, 1000));
    CHECK(loaded.load(bytes.bytes(), bytes.size()));
    CHECK(loaded.isComplete());
    CHECK(loaded.size() == 3);
    CHECK(loaded.totalBytes() == 700);
    loaded.put({ 14, 1, 5 }, 700);
    CHECK(loaded.nextEviction(tile));
    CHECK(tile == TileKey({ 14, 1, 3 }));
    loaded.erase(tile);
    // Over 90 % still: the next one goes too
    CHECK(loaded.nextEviction(tile));
    CHECK(tile == TileKey({ 14, 1, 4 }));
    CHECK(!loaded.load(bytes.bytes(), bytes.size() - 1));
    CHECK(loaded.size() == 0 && !loaded.isComplete());

    // A full index evicts too, even below the byte cap
    CacheIndex small;
    CHECK(small.init(4, 1000000));
    for (int i = 0; i < 4; i++) {
        small.put({ 10, i, 0 }, 10);
    }
    CHECK(small.isFull());
    small.put({ 10, 9, 9 }, 10);
    CHECK(!small.contains({ 10, 9, 9 }));
    CHECK(small.nextEviction(tile));
    CHECK(tile == TileKey({ 10, 0, 0 }));
}

void testCacheWalker() {
    printf("cache walker\n");
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "tactilitymaps-cache";
    std::error_code error;
    fs::remove_all(root, error);
    const uint8_t bytes[100] = {};
    // The current style, an old style, the old layout without a style folder, and map data that must stay
    CHECK(writeBytes(root / "cache" / "c2" / "14" / "9058" / "5729", bytes, 100));
    CHECK(writeBytes(root / "cache" / "c2" / "14" / "9058" / "5730", bytes, 50));
    CHECK(writeBytes(root / "cache" / "c2" / "13" / "4529" / "2864", bytes, 10));
    CHECK(writeBytes(root / "cache" / "c1" / "14" / "9058" / "5729", bytes, 100));
    CHECK(writeBytes(root / "cache" / "c1" / "index.bin", bytes, 16));
    CHECK(writeBytes(root / "cache" / "14" / "9058" / "5729", bytes, 100));
    CHECK(writeBytes(root / "cache" / "14" / "9058" / "5729.tmp", bytes, 100));
    CHECK(writeBytes(root / "maps" / "city" / "14" / "9058" / "5729.pbf", bytes, 100));
    CHECK(writeBytes(root / "maps" / "city" / "14" / "9058" / "5729.prt", bytes, 100));

    CacheIndex index;
    CHECK(index.init(100));
    CacheWalker walker;
    // Forward slashes, as on the device: the cache guard refuses backslashes
    const std::string rootText = root.generic_string();
    walker.begin(rootText.c_str(), "c2");
    int steps = 0;
    while (walker.step(index) && steps < 1000) {
        steps++;
    }
    CHECK(walker.isComplete());
    CHECK(index.isComplete());
    CHECK(index.size() == 3);
    CHECK(index.totalBytes() == 160);
    CHECK(index.contains({ 14, 9058, 5730 }));
    CHECK(fs::exists(root / "cache" / "c2" / "14" / "9058" / "5729"));
    CHECK(!fs::exists(root / "cache" / "c1"));
    CHECK(!fs::exists(root / "cache" / "14"));
    CHECK(fs::exists(root / "maps" / "city" / "14" / "9058" / "5729.pbf"));
    CHECK(fs::exists(root / "maps" / "city" / "14" / "9058" / "5729.prt"));

    // A complete index is kept as it is, its folder is not walked again
    walker.begin(rootText.c_str(), "c2");
    index.erase({ 14, 9058, 5730 });
    while (walker.step(index)) {
    }
    CHECK(index.size() == 2);

    // With the SD card cache switched off the current style's files go as well, a few per step
    CHECK(writeBytes(root / "cache" / "c2" / "14" / "9058" / "5731", bytes, 10));
    CHECK(writeBytes(root / "cache" / "c2" / "14" / "9058" / "5732", bytes, 10));
    CHECK(writeBytes(root / "cache" / "c2" / "14" / "9058" / "5733", bytes, 10));
    CHECK(writeBytes(root / "cache" / "c2" / "14" / "9058" / "5734", bytes, 10));
    CHECK(writeBytes(root / "cache" / "c2" / "14" / "9058" / "5735", bytes, 10));
    CHECK(writeBytes(root / "cache" / "c2" / "index.bin", bytes, 16));
    CacheIndex unused;
    CHECK(unused.init(10));
    CacheWalker cleaner;
    cleaner.begin(rootText.c_str(), "c2", false);
    int cleanSteps = 0;
    while (cleaner.step(unused) && cleanSteps < 1000) {
        cleanSteps++;
    }
    CHECK(cleanSteps >= 3);
    CHECK(!fs::exists(root / "cache" / "c2"));
    CHECK(fs::exists(root / "maps" / "city" / "14" / "9058" / "5729.pbf"));

    // Without a cache folder the walk ends at once with an empty, complete index
    CacheIndex empty;
    CHECK(empty.init(10));
    CacheWalker none;
    none.begin((root / "nothing").generic_string().c_str(), "c2");
    while (none.step(empty)) {
    }
    CHECK(empty.isComplete() && empty.size() == 0);
    fs::remove_all(root, error);
}

// ---------------------------------------------------------------------------------------------------------------
// Touch gestures, live pinch scaling
// ---------------------------------------------------------------------------------------------------------------

struct GestureLog {
    Gestures gestures;
    std::vector<Gesture> seen;

    void feed(uint8_t count, int x0, int y0, int x1, int y1, uint32_t now) {
        const TouchSample sample { count, { x0, x1 }, { y0, y1 } };
        Gesture out[Gestures::MAX_GESTURES];
        const int found = gestures.update(sample, now, out);
        for (int i = 0; i < found; i++) {
            seen.push_back(out[i]);
        }
    }

    void touch(int x, int y, uint32_t now) { feed(1, x, y, 0, 0, now); }
    void release(uint32_t now) { feed(0, 0, 0, 0, 0, now); }

    int countOf(GestureType type) const {
        int count = 0;
        for (const Gesture& gesture : seen) {
            count += gesture.type == type ? 1 : 0;
        }
        return count;
    }
};

void testGestures() {
    printf("gestures\n");
    {
        // A single tap: Tap at once, SingleTap after the double tap time
        GestureLog log;
        log.touch(100, 100, 0);
        log.touch(102, 101, 50);
        log.release(100);
        CHECK(log.countOf(GestureType::Tap) == 1);
        CHECK(log.countOf(GestureType::SingleTap) == 0);
        log.release(200);
        CHECK(log.countOf(GestureType::SingleTap) == 0);
        log.release(100 + Gestures::DOUBLE_TAP_MS);
        CHECK(log.countOf(GestureType::SingleTap) == 1);
        CHECK(log.seen.back().x == 102 && log.seen.back().y == 101);
        CHECK(log.countOf(GestureType::Drag) == 0);
    }
    {
        // A double tap is never a single tap
        GestureLog log;
        log.touch(100, 100, 0);
        log.release(60);
        log.touch(105, 98, 150);
        log.release(200);
        log.release(1000);
        CHECK(log.countOf(GestureType::Tap) == 2);
        CHECK(log.countOf(GestureType::DoubleTap) == 1);
        CHECK(log.countOf(GestureType::SingleTap) == 0);
        // Two taps far apart are two single taps
        GestureLog apart;
        apart.touch(10, 10, 0);
        apart.release(50);
        apart.touch(200, 200, 100);
        apart.release(150);
        apart.release(1000);
        CHECK(apart.countOf(GestureType::DoubleTap) == 0);
        CHECK(apart.countOf(GestureType::SingleTap) == 1);
    }
    {
        // A tap used by a button does not count as a single tap later
        GestureLog log;
        log.touch(100, 100, 0);
        log.release(50);
        log.gestures.consumeTap();
        log.release(1000);
        CHECK(log.countOf(GestureType::SingleTap) == 0);
    }
    {
        // A drag moves with the finger, including the part inside the slop, and is no tap
        GestureLog log;
        log.touch(100, 100, 0);
        log.touch(105, 100, 20);
        log.touch(120, 110, 40);
        log.touch(130, 110, 60);
        log.release(80);
        log.release(1000);
        int dx = 0;
        int dy = 0;
        for (const Gesture& gesture : log.seen) {
            if (gesture.type == GestureType::Drag) {
                dx += gesture.dx;
                dy += gesture.dy;
            }
        }
        CHECK(dx == 30 && dy == 10);
        CHECK(log.countOf(GestureType::Tap) == 0);
        CHECK(log.countOf(GestureType::SingleTap) == 0);
    }
    {
        // A long press is no tap
        GestureLog log;
        log.touch(100, 100, 0);
        log.touch(101, 100, Gestures::LONG_PRESS_MS + 10);
        log.touch(101, 100, Gestures::LONG_PRESS_MS + 50);
        log.release(Gestures::LONG_PRESS_MS + 100);
        log.release(3000);
        CHECK(log.countOf(GestureType::LongPress) == 1);
        CHECK(log.countOf(GestureType::Tap) == 0);
        CHECK(log.countOf(GestureType::SingleTap) == 0);
    }
    {
        // A pinch: start, moves with the scale, end; the finger left behind neither drags nor taps
        GestureLog log;
        log.touch(100, 100, 0);
        log.feed(2, 100, 100, 200, 100, 20);
        log.feed(2, 50, 100, 250, 100, 40);
        log.touch(50, 100, 60);
        log.touch(10, 100, 80);
        log.release(100);
        log.release(1000);
        CHECK(log.countOf(GestureType::PinchStart) == 1);
        CHECK(log.countOf(GestureType::PinchMove) == 1);
        CHECK(log.countOf(GestureType::PinchEnd) == 1);
        CHECK(log.countOf(GestureType::Drag) == 0);
        CHECK(log.countOf(GestureType::Tap) == 0);
        CHECK(log.countOf(GestureType::SingleTap) == 0);
        for (const Gesture& gesture : log.seen) {
            if (gesture.type == GestureType::PinchEnd) {
                CHECK(nearlyEqual(gesture.scale, 2.0, 1e-3));
                CHECK(gesture.startX == 150 && gesture.x == 150);
            }
        }
    }

    // Zoom snapping after a pinch
    CHECK(snapZoom(14, 1.0f, 19) == 14);
    CHECK(snapZoom(14, 1.3f, 19) == 14);
    CHECK(snapZoom(14, 1.5f, 19) == 15);
    CHECK(snapZoom(14, 3.0f, 19) == 16);
    CHECK(snapZoom(14, 0.6f, 19) == 13);
    CHECK(snapZoom(19, 4.0f, 19) == 19);
    CHECK(snapZoom(0, 0.1f, 19) == 0);

    // Scaling the screen picture around the pinch centre
    constexpr int W = 8;
    constexpr int H = 6;
    uint16_t source[W * H];
    for (int i = 0; i < W * H; i++) {
        source[i] = static_cast<uint16_t>(i);
    }
    uint16_t target[W * H];
    scaleView(source, target, W, H, 1.0f, 4.0f, 3.0f, 4.0f, 3.0f, 0xFFFF);
    CHECK(memcmp(source, target, sizeof(source)) == 0);
    // Twice as large around the centre: the centre pixel stays, its neighbours spread out
    scaleView(source, target, W, H, 2.0f, 4.0f, 3.0f, 4.0f, 3.0f, 0xFFFF);
    CHECK(target[3 * W + 4] == source[3 * W + 4]);
    CHECK(target[3 * W + 5] == source[3 * W + 4]);
    CHECK(target[3 * W + 6] == source[3 * W + 5]);
    CHECK(target[0] == source[1 * W + 2]);
    // Half the size around the centre of pixel 4, 3: it stays, its neighbours come from two pixels away
    scaleView(source, target, W, H, 0.5f, 4.5f, 3.5f, 4.5f, 3.5f, 0xFFFF);
    CHECK(target[0] == 0xFFFF);
    CHECK(target[3 * W + 4] == source[3 * W + 4]);
    CHECK(target[3 * W + 5] == source[3 * W + 6]);
    CHECK(target[3 * W + 3] == source[3 * W + 2]);
    CHECK(target[2 * W + 4] == source[1 * W + 4]);
    // Moved along with the fingers
    scaleView(source, target, W, H, 1.0f, 4.0f, 3.0f, 5.0f, 3.0f, 0xFFFF);
    CHECK(target[3 * W + 5] == source[3 * W + 4]);
    CHECK(target[3 * W + 0] == 0xFFFF);
}

// ---------------------------------------------------------------------------------------------------------------
// UI toolkit
// ---------------------------------------------------------------------------------------------------------------

void testWidgets() {
    printf("widgets\n");
    FakeFont font;
    std::vector<uint16_t> pixels(320 * 240, 0);
    Canvas canvas { pixels.data(), 320, 240 };

    // Text wraps at spaces and never runs past the width
    char lines[3][80];
    const int count = wrapText(font, "Location is off. Enable it in Tactility's Settings.", 200, &lines[0][0], sizeof(lines[0]), 3);
    CHECK(count == 3);
    for (int i = 0; i < count; i++) {
        CHECK(font.textWidth(lines[i]) <= 200);
    }
    CHECK(strcmp(lines[0], "Location is off. Enable") == 0);
    CHECK(wrapText(font, "short", 200, &lines[0][0], sizeof(lines[0]), 3) == 1);
    CHECK(strcmp(lines[0], "short") == 0);

    // A full screen list: about six rows on 240 px, keys move and choose, the selection stays visible
    ListView list;
    list.setTitle("< Regions");
    list.setHint("hint");
    for (int i = 0; i < 12; i++) {
        char label[16];
        snprintf(label, sizeof(label), "row %d", i);
        list.add(label, "1 MB");
    }
    list.layoutFullScreen(320, 240, font);
    CHECK(list.rowHeight() >= font.lineHeight() + 8);
    const int visible = (240 - list.rowHeight()) / list.rowHeight();
    CHECK(visible >= 5 && visible <= 8);
    CHECK(list.handleKey('s') == UiAction::Moved);
    CHECK(list.handleKey(keys::DOWN) == UiAction::Moved);
    CHECK(list.selected() == 2);
    CHECK(list.handleKey('w') == UiAction::Moved);
    CHECK(list.selected() == 1);
    CHECK(list.handleKey(keys::UP) == UiAction::Moved);
    CHECK(list.handleKey(keys::UP) == UiAction::None);
    CHECK(list.handleKey(keys::ENTER) == UiAction::Chosen);
    CHECK(list.handleKey(keys::ESCAPE) == UiAction::Back);
    CHECK(list.handleKey(keys::BACKSPACE) == UiAction::Back);
    CHECK(list.handleKey(keys::DELETE) == UiAction::Delete);
    CHECK(list.move(100) == UiAction::Moved);
    CHECK(list.selected() == 11);
    // The selected row is on screen, so a tap on it finds it
    bool found = false;
    for (int y = 0; y < 240 && !found; y++) {
        found = list.rowAt(100, y) == 11;
    }
    CHECK(found);
    CHECK(list.rowAt(100, 5) == -1);
    // A tap on the title goes back, a tap on a row chooses it
    CHECK(list.handleTap(100, 5) == UiAction::Back);
    int rowY = -1;
    for (int y = 0; y < 240 && rowY < 0; y++) {
        if (list.rowAt(100, y) >= 0 && list.rowAt(100, y) != 11) {
            rowY = y;
        }
    }
    const int tapped = list.rowAt(100, rowY);
    CHECK(list.handleTap(100, rowY) == UiAction::Chosen);
    CHECK(list.selected() == tapped);
    list.draw(canvas, font, uiTheme(colorPalette(), false));

    // A popup closes on a tap outside
    ListView menu;
    menu.add("Download area");
    menu.add("Regions");
    menu.add("Settings");
    menu.add("About and licences");
    menu.layoutPopup(320, 240, font, 220);
    CHECK(menu.bounds().width == 220);
    CHECK(menu.bounds().x == 50);
    CHECK(menu.handleTap(5, 5) == UiAction::Back);
    const Rect bounds = menu.bounds();
    CHECK(menu.rowAt(bounds.x + 20, bounds.y + bounds.height - 6) == 3);
    CHECK(menu.handleTap(bounds.x + 20, bounds.y + bounds.height - 6) == UiAction::Chosen);
    CHECK(menu.selected() == 3);
    CHECK(menu.handleKey(keys::DELETE) == UiAction::Delete);

    // A dialog starts on Cancel, so Enter alone never confirms
    Dialog dialog;
    dialog.open("Delete area01?", "Delete", true);
    dialog.layout(320, 240, font);
    CHECK(dialog.handleKey(keys::ENTER) == UiAction::Back);
    CHECK(!dialog.isOpen());
    dialog.open("Delete area01?", "Delete", true);
    CHECK(dialog.handleKey(keys::RIGHT) == UiAction::Moved);
    CHECK(dialog.handleKey(keys::ENTER) == UiAction::Chosen);
    dialog.open("Delete area01?", "Delete", true);
    CHECK(dialog.handleKey('y') == UiAction::Chosen);
    dialog.open("Delete area01?", "Delete", true);
    CHECK(dialog.handleKey(keys::ESCAPE) == UiAction::Back);
    dialog.open("Delete area01?", "Delete", true);
    dialog.layout(320, 240, font);
    CHECK(dialog.handleTap(2, 2) == UiAction::Back);
    dialog.draw(canvas, font, uiTheme(colorPalette(), false));

    // Monochrome: solid black and white, no grey text
    const UiTheme mono = uiTheme(monochromePalette(), true);
    CHECK(mono.text == 0x0000 && mono.panel == 0xFFFF);
    CHECK(mono.muted == mono.text);
    CHECK(mono.panelOpacity == 255);
}

// ---------------------------------------------------------------------------------------------------------------
// Control overlay and attribution
// ---------------------------------------------------------------------------------------------------------------

void testControls() {
    printf("controls\n");
    Controls controls;
    controls.layout(320, 240, 212);
    // Hidden at first: nothing to hit
    CHECK(!controls.isVisible());
    CHECK(controls.hit(20, 20) == Control::None);
    controls.toggle();
    CHECK(controls.isVisible());
    // Layout: about 32 px, at least 6 px from the edges, never over the screen centre
    for (int i = 0; i < static_cast<int>(Control::Count); i++) {
        const Rect& r = controls.rect(static_cast<Control>(i));
        CHECK(r.width == 32 && r.height == 32);
        CHECK(r.x >= 6 && r.y >= 6 && r.x + r.width <= 314 && r.y + r.height <= 234);
        CHECK(!r.contains(160, 120));
        for (int j = i + 1; j < static_cast<int>(Control::Count); j++) {
            const Rect& other = controls.rect(static_cast<Control>(j));
            const bool overlap = r.x < other.x + other.width && other.x < r.x + r.width && r.y < other.y + other.height && other.y < r.y + r.height;
            CHECK(!overlap);
        }
    }
    const Rect& menu = controls.rect(Control::Menu);
    CHECK(menu.x == 6 && menu.y == 6);
    const Rect& zoomIn = controls.rect(Control::ZoomIn);
    const Rect& zoomOut = controls.rect(Control::ZoomOut);
    CHECK(zoomIn.x == 320 - 6 - 32 && zoomIn.y + zoomIn.height <= 120 && zoomOut.y >= 120);
    const Rect& positionButton = controls.rect(Control::Position);
    CHECK(positionButton.x == zoomIn.x && positionButton.y + positionButton.height <= 212 - 6 + 1);
    CHECK(controls.hit(menu.x + 10, menu.y + 10) == Control::Menu);
    CHECK(controls.hit(zoomOut.x + 16, zoomOut.y + 16) == Control::ZoomOut);
    CHECK(controls.hit(160, 120) == Control::None);
    controls.toggle();
    CHECK(controls.hit(menu.x + 10, menu.y + 10) == Control::None);

    // Keys of the buttons
    CHECK(Controls::controlForKey('g') == Control::Position);
    CHECK(Controls::controlForKey('M') == Control::Menu);
    CHECK(Controls::controlForKey('x') == Control::None);
    CHECK(Controls::keyOf(Control::ZoomIn) == 'I');

    // Focus for boards with only buttons
    controls.setInputs({ true, false, false });
    CHECK(controls.usesFocus());
    controls.show();
    CHECK(controls.focused() == Control::Menu);
    controls.moveFocus(1);
    CHECK(controls.focused() == Control::ZoomIn);
    controls.moveFocus(-2);
    CHECK(controls.focused() == Control::Position);
    controls.setInputs({ true, true, false });
    CHECK(!controls.usesFocus());

    // Drawn in colour and monochrome without errors, with and without key hints
    FakeFont font;
    std::vector<uint16_t> pixels(320 * 240, 0x1234);
    Canvas canvas { pixels.data(), 320, 240 };
    controls.show();
    controls.draw(canvas, font, uiTheme(colorPalette(), false), PositionButton::Unavailable);
    controls.setInputs({ false, true, false });
    controls.draw(canvas, font, uiTheme(monochromePalette(), true), PositionButton::Following);
    CHECK(pixels[(menu.y + 16) * 320 + menu.x + 16] != 0x1234);
    CHECK(pixels[120 * 320 + 160] == 0x1234);

    // The attribution in two steps
    Attribution attribution;
    const Rect bar { 0, 220, 300, 18 };
    const Rect mark { 0, 222, 18, 18 };
    attribution.expand(1000);
    CHECK(attribution.isExpanded());
    CHECK(!attribution.update(5999));
    CHECK(attribution.update(6000));
    CHECK(!attribution.isExpanded());
    CHECK(attribution.tap(200, 100, bar, mark, 7000) == Attribution::Tap::Missed);
    CHECK(attribution.tap(5, 230, bar, mark, 7000) == Attribution::Tap::Expanded);
    CHECK(attribution.isExpanded());
    CHECK(attribution.tap(150, 225, bar, mark, 8000) == Attribution::Tap::OpenAbout);
    CHECK(attribution.update(12000));
}

// ---------------------------------------------------------------------------------------------------------------
// Download screen
// ---------------------------------------------------------------------------------------------------------------

void testDownloadScreen() {
    printf("download screen\n");
    const DownloadLayout layout = downloadLayout(320, 240, 16);
    // The frame fills the map area above the info bar, minus an even margin
    CHECK(layout.bar.y + layout.bar.height == 240);
    CHECK(layout.bar.height == 42);
    CHECK(layout.frame.x == 10 && layout.frame.y == 10);
    CHECK(320 - (layout.frame.x + layout.frame.width) == 10);
    CHECK(layout.bar.y - (layout.frame.y + layout.frame.height) == 10);
    CHECK(layout.button.contains(layout.button.x + 1, layout.button.y + 1));
    CHECK(layout.bar.contains(layout.button.x, layout.button.y));
    CHECK(layout.button.x + layout.button.width <= 320);

    // The ground under the frame follows the map: the same frame covers four times the area one zoom level out
    const double centerX = lonToWorldX(19.04, 14);
    const double centerY = latToWorldY(47.50, 14);
    const BoundingBox near = screenBounds(layout.frame, centerX, centerY, 14, 320, 240);
    const BoundingBox far = screenBounds(layout.frame, centerX / 2, centerY / 2, 13, 320, 240);
    CHECK(near.west < near.east && near.south < near.north);
    CHECK(nearlyEqual((far.east - far.west) / (near.east - near.west), 2.0, 1e-6));
    // The frame sits a little above the screen centre, the info bar takes the bottom
    CHECK(near.north > 47.50 && near.south < 47.50);
    // Panning moves the frame's ground with the map
    const BoundingBox moved = screenBounds(layout.frame, centerX + 100, centerY, 14, 320, 240);
    CHECK(moved.west > near.west);

    // Live numbers: a city frame fits, and the max zoom drops for a large one
    char first[48];
    char second[48];
    const DownloadPlan city = planDownload(near, MAX_SOURCE_ZOOM);
    describePlan(city, first, sizeof(first), second, sizeof(second));
    CHECK(city.fits && city.maxZoom == 14);
    CHECK(strstr(first, "tiles") != nullptr);
    CHECK(strcmp(second, "Max zoom 14") == 0);
    const BoundingBox wide = screenBounds(layout.frame, lonToWorldX(19.04, 9), latToWorldY(47.5, 9), 9, 320, 240);
    const DownloadPlan country = planDownload(wide, MAX_SOURCE_ZOOM);
    describePlan(country, first, sizeof(first), second, sizeof(second));
    CHECK(!country.fits || country.maxZoom < 14);
    if (country.fits) {
        CHECK(strstr(second, "to fit") != nullptr);
        CHECK(country.tiles <= MAX_DOWNLOAD_TILES && country.estimatedBytes <= MAX_DOWNLOAD_BYTES);
    }
    DownloadPlan tooLarge {};
    tooLarge.fits = false;
    describePlan(tooLarge, first, sizeof(first), second, sizeof(second));
    CHECK(strstr(first, "Too large") != nullptr);

    FakeFont font;
    std::vector<uint16_t> pixels(320 * 240, 0x1234);
    Canvas canvas { pixels.data(), 320, 240 };
    drawDownloadScreen(canvas, font, uiTheme(colorPalette(), false), layout, city);
    // Inside the frame the map is untouched, outside it is dimmed
    CHECK(pixels[120 * 320 + 160] == 0x1234);
    CHECK(pixels[3 * 320 + 3] != 0x1234);
}

// ---------------------------------------------------------------------------------------------------------------
// Position from the location service
// ---------------------------------------------------------------------------------------------------------------

void testPosition() {
    printf("position\n");
    FakePosition source;
    PositionTracker tracker;

    // Location switched off in the system settings
    source.enabled = false;
    CHECK(tracker.update(source));
    CHECK(tracker.state() == PositionState::Off);
    CHECK(!tracker.hasFix());
    CHECK(strstr(tracker.unavailableMessage(), "off") != nullptr);
    CHECK(!tracker.update(source));

    // On, no fix yet
    source.enabled = true;
    CHECK(tracker.update(source));
    CHECK(tracker.state() == PositionState::Waiting);
    CHECK(strstr(tracker.unavailableMessage(), "No position") != nullptr);
    CHECK(tracker.accuracyRadius(10.0) == 0.0f);

    // An approximate fix: a circle, never an arrow, even with a course
    source.set(47.4979, 19.0402, 3000.0f, 90.0f, 5.0f);
    CHECK(tracker.update(source));
    CHECK(tracker.state() == PositionState::Approximate);
    CHECK(tracker.hasFix());
    CHECK(!tracker.showsArrow());
    CHECK(nearlyEqual(tracker.accuracyRadius(10.0), 300.0, 1e-3));
    CHECK(!tracker.update(source));

    // A precise fix while standing still: no arrow
    source.set(47.5, 19.05, 8.0f, NAN, 0.2f);
    CHECK(tracker.update(source));
    CHECK(tracker.state() == PositionState::Precise);
    CHECK(!tracker.showsArrow());
    CHECK(nearlyEqual(tracker.fix().latitude, 47.5, 1e-9));

    // Moving with a known course: the arrow shows
    source.set(47.5, 19.05, 8.0f, 45.0f, 3.0f);
    CHECK(tracker.update(source));
    CHECK(tracker.showsArrow());
    // A course without a speed is not enough
    source.set(47.5, 19.05, 8.0f, 45.0f, NAN);
    CHECK(tracker.update(source));
    CHECK(!tracker.showsArrow());

    // Unknown accuracy: precise, no circle
    source.set(47.5, 19.05, NAN);
    tracker.update(source);
    CHECK(tracker.state() == PositionState::Precise);
    CHECK(tracker.accuracyRadius(1.0) == 0.0f);

    // A fix with unknown coordinates is no fix
    source.set(NAN, NAN, 5.0f);
    tracker.update(source);
    CHECK(!tracker.hasFix());

    // Switching location off drops the fix
    source.set(47.5, 19.05, 5.0f);
    tracker.update(source);
    CHECK(tracker.hasFix());
    source.enabled = false;
    CHECK(tracker.update(source));
    CHECK(!tracker.hasFix());
    CHECK(tracker.state() == PositionState::Off);
}

int main(int argc, char** argv) {
    bool update = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--update") == 0) {
            update = true;
        }
    }

    testTileMath();
    testPaths();
    testMvtDecoding();
    testInflate();
    testRaster();
    testGolden(update);
    testCompose();
    testTileCodec();
    testDownloadPlan();
    testRegionMeta();
    testTileJson();
    testPosition();
    testPreparedTiles();
    testPreparedScanner();
    testSettings();
    testTileCache();
    testIoQueue();
    testCacheIndex();
    testCacheWalker();
    testGestures();
    testWidgets();
    testControls();
    testDownloadScreen();

    printf("%d checks, %d failed\n", checkCount, failureCount);
    return failureCount == 0 ? 0 : 1;
}
