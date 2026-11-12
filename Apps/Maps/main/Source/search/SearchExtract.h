// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/MvtDecoder.h"
#include "../route/GrowArray.h"
#include "AddressMatch.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/** "No string" in the offsets below. */
constexpr uint32_t EXTRACT_NONE = 0xFFFFFFFFu;
/** ExtractPoint::settlement of a point that is not a settlement. */
constexpr uint8_t EXTRACT_NOT_SETTLEMENT = 255;

/**
 * A point of the search index as the M6 study makes it (host/search/data.py load_region): a place, a POI (named, or
 * unnamed and not in the junk list), a water label, a peak or a park. Positions are global z14 units (tile x * 4096 +
 * local x after scaling by the layer's extent). Strings are offsets into ExtractedTile::text.
 */
struct ExtractPoint {
    /** "place/town", "poi/cafe/cafe", "water/lake", "peak/peak", "park/park" ... */
    uint32_t label;
    /** The display name (name, else name:latin, else name_en), EXTRACT_NONE for an unnamed POI. */
    uint32_t name;
    /** "name:latin" or EXTRACT_NONE. */
    uint32_t latin;
    /** "name_en" or EXTRACT_NONE. */
    uint32_t en;
    int32_t x;
    int32_t y;
    /** A PlaceClass for the village rule (city town village hamlet isolated_dwelling suburb quarter neighbourhood), else EXTRACT_NOT_SETTLEMENT. */
    uint8_t settlement;
    /** Whether name_en makes search keys: places, water, peaks and parks, not POIs (the study's rule). */
    uint8_t useEn;
};

/** A named line of the transportation_name layer (every class, paths and pedestrian streets too). */
struct ExtractLine {
    uint32_t name;
    uint32_t latin;
    /** Index of the first point in ExtractedTile::linePoints (x, y pairs), and the point count (at least 2). */
    uint32_t first;
    uint32_t count;
    /** Class path, track, aerialway, ferry or rail (the study's PATHCLS, which its street matching left out). */
    uint8_t pathClass;
};

struct ExtractHouse {
    uint32_t text;
    int32_t x;
    int32_t y;
    /** The position is inside the tile (not in its buffer). */
    uint8_t inside;
};

/** What one tile gives. The arrays keep their memory between tiles (clear() only empties them). */
struct ExtractedTile {
    int tileX = 0;
    int tileY = 0;
    /** Zero-terminated strings. */
    GrowArray<char> text;
    GrowArray<ExtractPoint> points;
    GrowArray<ExtractLine> lines;
    GrowArray<int32_t> linePoints;
    GrowArray<ExtractHouse> houses;
    /** Lines of the transportation_name layer that have no name at all (only a ref). */
    uint32_t refOnlyLines = 0;
    /** Unnamed POIs of the junk list that were not kept. */
    uint32_t junkPois = 0;

    void clear();
    void release();
    const char* str(uint32_t offset) const { return offset == EXTRACT_NONE ? nullptr : &text[offset]; }
    bool failed() const { return text.failed() || points.failed() || lines.failed() || linePoints.failed() || houses.failed(); }
};

/**
 * Takes what the extractor finds feature by feature, for the streaming index build: the tile's arrays then hold only
 * one feature's point, house number or lines (a line feature may have several parts) and are emptied after take().
 */
class ExtractConsumer {
public:
    virtual ~ExtractConsumer() = default;
    /** False stops the extraction (extract() then returns false). */
    virtual bool take(const ExtractedTile& feature) = 0;
};

/**
 * Decodes one z14 tile and collects what the search index needs from the layers place, poi, transportation_name,
 * housenumber, water_name, mountain_peak and park. Platform free; the decoder and the arrays are the caller's, so a
 * whole region reuses them.
 */
class TileExtractor final : public GeometrySink {
public:
    /**
     * Decodes the tile (the ByteInput of its .pbf) into out. Returns false when the tile cannot be read. With a
     * consumer, each feature is handed over as soon as it is decoded and out stays small (the counters keep counting).
     */
    bool extract(MvtDecoder& decoder, ByteInput* input, int tileX, int tileY, ExtractedTile& out, ExtractConsumer* consumer = nullptr);

    bool beginFeature(const FeatureInfo& info) override;
    void beginRing() override;
    void points(const TilePoint* chunk, int count) override;
    void endRing(bool closed) override;
    void endFeature() override;

private:
    enum class Shape : uint8_t { None, Point, Line, House };

    uint32_t addText(const char* text, size_t length);
    uint32_t addLabel(const char* kind, const char* classText, size_t classLength, const char* subclassText, size_t subclassLength);
    int32_t scaled(int32_t value, int32_t origin) const;

    ExtractedTile* out = nullptr;
    ExtractConsumer* consumer = nullptr;
    bool consumerOk = true;
    int originX = 0;
    int originY = 0;
    int32_t extents[static_cast<int>(Layer::Count)] = {};
    // The feature being decoded
    Shape shape = Shape::None;
    int32_t extent = 4096;
    uint32_t label = EXTRACT_NONE;
    uint32_t name = EXTRACT_NONE;
    uint32_t latin = EXTRACT_NONE;
    uint32_t en = EXTRACT_NONE;
    uint8_t settlement = EXTRACT_NOT_SETTLEMENT;
    uint8_t useEn = 0;
    uint8_t pathClass = 0;
    int rings = 0;
    int chunks = 0;
    uint32_t lineStart = 0;
    bool havePoint = false;
    int32_t pointX = 0;
    int32_t pointY = 0;
    bool inside = false;
    /** The first ring of a point-like feature (a park's outline is only needed for its middle vertex). */
    GrowArray<TilePoint> ring;
};

} // namespace maps
