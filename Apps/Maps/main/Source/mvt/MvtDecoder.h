// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "ByteInput.h"
#include "InflateReader.h"
#include "Schema.h"

#include <cstdint>

namespace maps {

struct TilePoint {
    int32_t x;
    int32_t y;
};

/**
 * Receives decoded features. Each ring or line arrives in chunks of points between beginRing() and endRing(),
 * so no complete geometry is ever buffered.
 */
class GeometrySink {
public:
    virtual ~GeometrySink() = default;
    /** Returns false to skip the geometry of this feature. */
    virtual bool beginFeature(const FeatureInfo& info) = 0;
    virtual void beginRing() = 0;
    virtual void points(const TilePoint* points, int count) = 0;
    virtual void endRing(bool closed) = 0;
    virtual void endFeature() = 0;
    /** Checked before each feature: true ends the sweep early, which then fails. */
    virtual bool shouldStop() { return false; }
};

/**
 * Streaming Mapbox Vector Tile (2.1) decoder. scan() reads the key and value tables of the wanted layers,
 * which come after the features in each layer, then each sweep() decodes features of some layers in file order.
 * Every pass inflates the tile again from the start instead of keeping the decompressed tile in memory.
 */
class MvtDecoder {
public:
    static constexpr int CHUNK_POINTS = 256;
    static constexpr int VALUE_POOL_SIZE = 8192;
    static constexpr int NAME_POOL_SIZE = 24576;
    /**
     * The pools are this big when the poi and housenumber layers are scanned: every tag value of them counts. Those
     * layers come last in OpenMapTiles tiles, so in the densest tiles only some of their strings miss the pools (those
     * POIs lose their names), never a place or street name. 96 KB + 160 KB, given back after each conversion: a
     * 384 KB name pool took the T-Deck's largest free PSRAM block and the label section could not grow (device test,
     * 10 Nov 2026).
     */
    static constexpr int POI_VALUE_POOL_SIZE = 12288;
    static constexpr int POI_NAME_POOL_SIZE = 163840;

    MvtDecoder() = default;
    ~MvtDecoder();
    MvtDecoder(const MvtDecoder&) = delete;
    MvtDecoder& operator=(const MvtDecoder&) = delete;

    bool init();
    /**
     * Reads the tables of the layers. The poi and housenumber layers are skipped, as if the tile had none, unless
     * withPoiLayers is set: only the prepared tile converter wants them, and their strings fill the pools.
     */
    bool scan(ByteInput* input, bool withPoiLayers = false);
    /** Back to the normal pool sizes after a scan with the POI layers. */
    void shrinkPools();
    /** Decodes the features of the given layers, in the order they appear in the tile. */
    bool sweep(const Layer* layers, int count, GeometrySink& sink);

    /** Position of a layer in the tile, -1 when the tile does not have it. */
    int fileIndex(Layer layer) const { return tables[static_cast<int>(layer)].fileIndex; }
    int extent(Layer layer) const { return tables[static_cast<int>(layer)].extent; }
    size_t decompressedSize() const { return totalSize; }
    /** Inflates a whole stream into out with the decoder's decompressor. Returns false unless exactly size bytes came out. */
    bool inflate(ByteInput* source, uint8_t* out, size_t size);
    InflateReader::Format format() const { return reader.format(); }

private:
    struct ValueEntry {
        uint16_t code;
        uint8_t textLength;
        uint8_t hasText;
        union {
            uint32_t textOffset;
            int32_t rank;
        };
    };

    struct LayerTable {
        int8_t fileIndex = -1;
        int16_t classKey = -1;
        int16_t subclassKey = -1;
        int16_t brunnelKey = -1;
        int16_t adminLevelKey = -1;
        int16_t maritimeKey = -1;
        int16_t rampKey = -1;
        int16_t onewayKey = -1;
        int16_t layerKey = -1;
        int16_t nameKey = -1;
        int16_t nameLatinKey = -1;
        /** "name_en" and "name:en": a layer can have both, on different features; name_en wins on a feature with both. */
        int16_t nameEnKey = -1;
        int16_t nameEnColonKey = -1;
        int16_t rankKey = -1;
        int16_t houseNumberKey = -1;
        uint32_t valueOffset = 0;
        uint32_t valueCount = 0;
        int32_t extent = 4096;
    };

    bool growPools();
    bool readVarint(uint64_t& value);
    bool readLayerName(size_t layerEnd, Layer& layer);
    bool scanLayer(size_t layerEnd, LayerTable& table, Layer layer);
    bool scanValue(size_t valueEnd, ValueEntry& entry);
    bool decodeFeature(size_t featureEnd, Layer layer, GeometrySink& sink);
    void applyTag(const LayerTable& table, uint32_t key, uint32_t value, FeatureInfo& info) const;
    bool decodeGeometry(size_t geometryEnd, GeometrySink& sink);
    bool skipField(uint32_t wireType);
    const ValueEntry* valueEntry(const LayerTable& table, uint32_t index) const;

    InflateReader reader;
    ByteInput* input = nullptr;
    LayerTable tables[static_cast<int>(Layer::Count)] {};
    ValueEntry* valuePool = nullptr;
    uint32_t valuePoolUsed = 0;
    uint32_t valuePoolCapacity = 0;
    char* namePool = nullptr;
    uint32_t namePoolUsed = 0;
    uint32_t namePoolCapacity = 0;
    TilePoint* chunk = nullptr;
    size_t totalSize = 0;
};

} // namespace maps
