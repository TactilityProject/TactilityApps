// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/MvtDecoder.h"
#include "../tiles/TileMath.h"
#include "Style.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Prepared tiles: a render-ready copy of a vector tile, stored next to the untouched original as <y>.prt.
 * It keeps only the features the style can draw at the zoom levels the tile is used for, with only the
 * attributes the style reads, coordinates quantised to PREPARED_EXTENT units per tile, grouped by draw stage.
 * Raising PREPARED_FORMAT_VERSION makes the app regenerate every prepared tile from the originals. Raise it
 * when the style draws features it used to skip, or when the order of the Value list in Schema.h changes, since
 * classes are stored as Value numbers.
 *
 * Layout (little endian): a 64-byte header, then the body, raw or as a zlib stream.
 *   header: "TMPT", u16 version, u16 flags, u8 z, u8 0, u16 extent, u32 x, u32 y, u32 source size,
 *           u32 body size, u32 stored body size, char source version[32]
 *   body:   sections, each u8 stage, varint feature count, varint byte length, features; then, when the tile
 *           has labels, a label section (u8 PREPARED_LABEL_SECTION, varint label count, varint byte length,
 *           labels); then 0xFF. The RoadCasing section is drawn again for the RoadFill stage.
 *   feature: u8 flags (bits 0-1 geometry type, 2 maritime, 3 ramp, 4 has subclass, 5 has brunnel,
 *           6 has admin level), varint class, [varint subclass], [varint brunnel], [u8 admin level],
 *           varint ring count, rings: varint (point count << 1 | closed), zigzag varint deltas x, y.
 *           Deltas continue across the rings of a feature and start at 0, 0.
 *   label:  u8 kind, u8 class, varint rank + 1 (0 when absent), varint name length, name bytes,
 *           varint latin name length (0 when absent or equal to name), latin name bytes,
 *           varint point count, zigzag varint deltas x, y. Deltas start at 0, 0 for each label.
 *           Kinds 0-4 are place, road, water, waterway and peak. Version 4 adds kind 5, poi (class = type id from
 *           PoiTypes.h, rank + 1, name at most 63 bytes, empty for types shown without a name, one point), and
 *           kind 6, house number (class 0, no rank, the number as the name, no latin name, one point). Both come
 *           after the other labels. Version 3 tiles have neither and are still readable.
 */
constexpr uint16_t PREPARED_FORMAT_VERSION = 4;
constexpr uint16_t PREPARED_MIN_READABLE_VERSION = 2;
constexpr uint8_t PREPARED_LABEL_SECTION = 10;
// 2 px per unit at z18 from z14 data. 1024 looked stepped at z17-z19; 4096 made deflated tiles 35 % larger
// (docs/MEASUREMENTS.md, M3.2).
constexpr int PREPARED_EXTENT = 2048;
constexpr size_t PREPARED_HEADER_SIZE = 64;
constexpr size_t PREPARED_VERSION_LENGTH = 32;
constexpr uint16_t PREPARED_FLAG_DEFLATED = 1;

struct PreparedHeader {
    uint16_t version;
    uint16_t flags;
    TileKey tile;
    int extent;
    uint32_t sourceSize;
    uint32_t bodySize;
    uint32_t storedSize;
    char sourceVersion[PREPARED_VERSION_LENGTH + 1];
};

bool readPreparedHeader(const uint8_t* data, size_t size, PreparedHeader& header);

/** Whether a prepared tile was made by this format version from this original. */
bool isPreparedCurrent(const PreparedHeader& header, const TileKey& tile, uint32_t sourceSize);

/** Whether a prepared tile can still be drawn, even when it was made by an older format version. */
bool isPreparedReadable(const PreparedHeader& header, const TileKey& tile, uint32_t sourceSize);

/** A byte buffer that grows as needed. */
class ByteWriter {
public:
    ByteWriter() = default;
    ~ByteWriter();
    ByteWriter(const ByteWriter&) = delete;
    ByteWriter& operator=(const ByteWriter&) = delete;

    void clear() {
        length = 0;
        error = false;
    }
    /** Clears and frees the buffer. */
    void release();
    bool reserve(size_t size);
    /** Room for exactly size bytes, no doubling: for a download whose length is known. */
    bool reserveExact(size_t size);
    void put(uint8_t value);
    void putBytes(const void* bytes, size_t count);
    void putVarint(uint64_t value);
    void putZigzag(int32_t value) { putVarint((static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31)); }
    void putU16(uint16_t value);
    void putU32(uint32_t value);
    void setU32(size_t offset, uint32_t value);

    const uint8_t* bytes() const { return data; }
    uint8_t* bytes() { return data; }
    size_t size() const { return length; }
    size_t allocated() const { return capacity; }
    bool failed() const { return error; }

private:
    uint8_t* data = nullptr;
    size_t capacity = 0;
    size_t length = 0;
    bool error = false;
};

struct ConvertStats {
    uint32_t totalMs;
    uint32_t featuresIn;
    uint32_t featuresKept;
    uint32_t pointsIn;
    uint32_t pointsKept;
    uint32_t bodyBytes;
    uint32_t storedBytes;
};

/**
 * Converts a vector tile into a prepared tile. It uses the decoder of a render lane, so a conversion needs no
 * second decompressor.
 */
class TileConverter final : private GeometrySink {
public:
    TileConverter() = default;
    ~TileConverter() override;
    TileConverter(const TileConverter&) = delete;
    TileConverter& operator=(const TileConverter&) = delete;

    /**
     * Writes the prepared tile of the vector tile in input to out. sourceSize is the size of the original file,
     * sourceVersion the tile set version it came from.
     */
    bool convert(MvtDecoder& decoder, ByteInput* input, const TileKey& tile, uint32_t sourceSize, const char* sourceVersion,
        bool deflate, ByteWriter& out, ConvertStats* stats);
    /** Why the last convert() failed. */
    const char* failure() const { return failureText; }
    /** Frees section buffers larger than keepBytes, which a large low-zoom tile leaves behind. */
    void trim(size_t keepBytes);

private:
    bool beginFeature(const FeatureInfo& info) override;
    void beginRing() override;
    void points(const TilePoint* points, int count) override;
    void endRing(bool closed) override;
    void endFeature() override;

    bool growRing(size_t points);
    int simplifyRing(int count, float tolerance);
    void writeLabel(const int32_t* points, int count);

    MvtDecoder* decoder = nullptr;
    const char* failureText = "";
    TileKey tile {};
    int minZoom = 0;
    int maxZoom = 0;
    float unitsPerSourceUnit = 1.0f;
    float pixelsPerUnitAt16 = 1.0f;
    ByteWriter sections[static_cast<int>(Stage::Count)];
    uint32_t sectionCounts[static_cast<int>(Stage::Count)] {};
    ByteWriter rings;
    ByteWriter labels;
    /** The poi and house number labels, written apart so that they cannot crowd out the others in labels. */
    ByteWriter extraLabels;
    size_t poiBytes = 0;
    size_t numberBytes = 0;
    uint32_t labelCount = 0;
    int section = 0;
    FeatureInfo feature {};
    uint32_t ringCount = 0;
    bool drawFeature = false;
    bool labelFeature = false;
    uint8_t labelKind = 0;
    int32_t lastX = 0;
    int32_t lastY = 0;
    float minX = 0.0f;
    float minY = 0.0f;
    float maxX = 0.0f;
    float maxY = 0.0f;
    float* ringPoints = nullptr;
    uint8_t* keep = nullptr;
    uint32_t* stack = nullptr;
    size_t ringCapacity = 0;
    size_t ringLength = 0;
    bool ringOverflow = false;
    ConvertStats counters {};
};

/** The section a stage draws from: road fills reuse the road casing section. */
inline Stage preparedSection(Stage stage) {
    return stage == Stage::RoadFill ? Stage::RoadCasing : stage;
}

/** Compresses data into a zlib stream. */

} // namespace maps
