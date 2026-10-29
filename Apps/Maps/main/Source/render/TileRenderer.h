// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/MvtDecoder.h"
#include "../tiles/TileMath.h"
#include "Raster.h"
#include "Style.h"

#include <atomic>
#include <cstdint>

namespace maps {

struct RenderStats {
    uint32_t totalMs;
    uint32_t scanMs;
    uint32_t sweeps;
    uint32_t features;
    uint32_t decompressedBytes;
    /** Time spent decoding the geometry of drawn features and adding their edges. */
    uint32_t geometryMs;
    /** Time spent turning coverage into pixels. */
    uint32_t fillMs;
    /** The same in microseconds, and the part of geometry and fill spent on buildings. */
    uint32_t geometryUs;
    uint32_t fillUs;
    uint32_t buildingUs;
};

/**
 * Renders one 256x256 RGB565 tile from a vector tile file. The source tile can be an ancestor of the target
 * tile, which is then drawn magnified from the part of the source that covers it (overzoom).
 */
class TileRenderer final : private GeometrySink {
public:
    TileRenderer() = default;
    ~TileRenderer() override;
    TileRenderer(const TileRenderer&) = delete;
    TileRenderer& operator=(const TileRenderer&) = delete;

    bool init();
    bool render(ByteInput* input, const TileKey& source, const TileKey& target, const Palette& palette, uint16_t* pixels, RenderStats* stats);
    /** Renders from a prepared tile (see PreparedTile.h), whose header names the source tile. */
    bool renderPrepared(const uint8_t* data, size_t size, const TileKey& target, const Palette& palette, uint16_t* pixels, RenderStats* stats);

    /** The decoder, lent to a TileConverter so a lane needs only one decompressor. */
    MvtDecoder& tileDecoder() { return decoder; }

    /** A flag that stops the running render when set, which then returns false. */
    void setCancelFlag(const std::atomic<bool>* flag) { cancelFlag = flag; }
    bool wasCancelled() const { return cancelled; }

private:
    void begin(const TileKey& source, const TileKey& target, const Palette& palette, uint16_t* out);
    void finishStats(RenderStats* stats, uint32_t start, uint32_t scanned, uint32_t sweeps) const;
    bool playSection(const uint8_t* data, const uint8_t* end, Stage drawStage);

    bool beginFeature(const FeatureInfo& info) override;
    void beginRing() override;
    void points(const TilePoint* points, int count) override;
    void endRing(bool closed) override;
    void endFeature() override;
    bool shouldStop() override;

    void addPoint(float x, float y);

    MvtDecoder decoder;
    Raster raster;
    const Palette* palette = nullptr;
    uint16_t* pixels = nullptr;
    Stage stage = Stage::Landcover;
    Stage layerStages[static_cast<int>(Layer::Count)] {};
    int dz = 0;
    int zoom = 0;
    float scale = 1.0f;
    float offsetX = 0.0f;
    float offsetY = 0.0f;
    DrawStyle style {};
    /** Buildings at native z14: aliased, and skipped when smaller than about 2 px. */
    bool cheapFeature = false;
    float boundsMinX = 0.0f;
    float boundsMinY = 0.0f;
    float boundsMaxX = 0.0f;
    float boundsMaxY = 0.0f;
    bool firstPoint = true;
    float startX = 0.0f;
    float startY = 0.0f;
    float lastX = 0.0f;
    float lastY = 0.0f;
    bool hasDirection = false;
    float lastDirectionX = 0.0f;
    float lastDirectionY = 0.0f;
    uint32_t featureCount = 0;
    uint64_t featureStartUs = 0;
    uint64_t geometryUs = 0;
    uint64_t fillUs = 0;
    uint64_t buildingUs = 0;
    /** Coordinate units per tile of prepared tiles, 0 while rendering a vector tile. */
    int preparedExtent = 0;
    uint8_t* body = nullptr;
    size_t bodyCapacity = 0;
    const std::atomic<bool>* cancelFlag = nullptr;
    bool cancelled = false;
};

} // namespace maps
