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

    MvtDecoder() = default;
    ~MvtDecoder();
    MvtDecoder(const MvtDecoder&) = delete;
    MvtDecoder& operator=(const MvtDecoder&) = delete;

    bool init();
    bool scan(ByteInput* input);
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
    struct LayerTable {
        int8_t fileIndex;
        int16_t classKey;
        int16_t subclassKey;
        int16_t brunnelKey;
        int16_t adminLevelKey;
        int16_t maritimeKey;
        int16_t rampKey;
        uint32_t valueOffset;
        uint32_t valueCount;
        int32_t extent;
    };

    bool readVarint(uint64_t& value);
    bool readLayerName(size_t layerEnd, Layer& layer);
    bool scanLayer(size_t layerEnd, LayerTable& table, Layer layer);
    bool scanValue(size_t valueEnd, uint16_t& code);
    bool decodeFeature(size_t featureEnd, Layer layer, GeometrySink& sink);
    void applyTag(const LayerTable& table, uint32_t key, uint32_t value, FeatureInfo& info) const;
    bool decodeGeometry(size_t geometryEnd, GeometrySink& sink);
    bool skipField(uint32_t wireType);
    uint16_t valueCode(const LayerTable& table, uint32_t index) const;

    InflateReader reader;
    ByteInput* input = nullptr;
    LayerTable tables[static_cast<int>(Layer::Count)] {};
    uint16_t* valuePool = nullptr;
    uint32_t valuePoolUsed = 0;
    TilePoint* chunk = nullptr;
    size_t totalSize = 0;
};

} // namespace maps
