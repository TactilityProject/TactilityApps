// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../mvt/ByteInput.h"
#include "../mvt/InflateReader.h"

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Elevation from AWS Terrain Tiles ("Terrarium" PNG, 256 x 256 RGB, height = R * 256 + G + B / 256 - 32768 m),
 * decoded as a stream: the PNG's IDAT chunks are inflated row by row and the row filters undone with two rows of
 * 768 bytes, so the RGB image is never held. Heights are kept as 16 bits in half metres from -11,000 m
 * (heightFromCode()), 128 KB for a tile.
 */
constexpr int TERRAIN_TILE_SIZE = 256;
constexpr float TERRAIN_BASE_M = -11000.0f;

inline uint16_t terrainCode(float metres) {
    const float code = (metres - TERRAIN_BASE_M) * 2.0f;
    return code <= 0.0f ? 0 : (code >= 65535.0f ? 65535 : static_cast<uint16_t>(code + 0.5f));
}

inline float heightFromCode(uint16_t code) {
    return static_cast<float>(code) * 0.5f + TERRAIN_BASE_M;
}

/** The IDAT payloads of a PNG as one stream (the zlib data), with the header read first. */
class PngDataInput final : public ByteInput {
public:
    explicit PngDataInput(ByteInput& file) : file(file) {}
    /** Reads the signature and IHDR; false when it is not an 8-bit RGB or RGBA, non-interlaced PNG. */
    bool open();
    int width() const { return imageWidth; }
    int height() const { return imageHeight; }
    /** Bytes per pixel: 3 (RGB) or 4 (RGBA). */
    int pixelBytes() const { return bytesPerPixel; }
    int read(uint8_t* buffer, size_t size) override;
    bool rewind() override;

private:
    bool readExact(uint8_t* buffer, size_t size);
    /** Moves to the next IDAT chunk; false at IEND or on a damaged file. */
    bool nextData();

    ByteInput& file;
    int imageWidth = 0;
    int imageHeight = 0;
    int bytesPerPixel = 0;
    /** Bytes left in the current IDAT chunk (its CRC follows). */
    uint32_t left = 0;
    bool ended = false;
};

/**
 * Decodes a Terrarium tile into grid (TERRAIN_TILE_SIZE rows of TERRAIN_TILE_SIZE codes, rows stride codes apart).
 * rowBuffer holds 2 x 1024 bytes (two rows of RGBA). False on a damaged or unexpected file.
 */
bool decodeTerrarium(ByteInput& file, InflateReader& inflater, uint8_t* rowBuffer, uint16_t* grid, int stride);

/**
 * Hillshade of a height grid, light 45 degrees up from lightAzimuth (degrees clockwise from north; 315, the
 * north-west, is the usual map convention: the top left of a north-up screen): one byte per cell, 0 where a slope
 * faces the light or the ground is flat, up to 255 for the darkest shade. metresPerCell is the ground size of one cell
 * (Web Mercator: smaller towards the poles); exaggeration steepens the slopes for a small screen. Border cells use
 * their inner neighbours.
 */
void hillshade(const uint16_t* grid, int width, int height, int stride, float metresPerCell, float exaggeration, uint8_t* shade, int shadeStride,
    float lightAzimuth = 315.0f);

/**
 * The light step for a map turned by bearingDegrees (the direction at the top of the screen): the bearing rounded to
 * 45 degrees, 0-7. The light then comes from the top left of the screen as on a north-up map, at most 22.5 degrees
 * off, and tiles are drawn again only when the step changes.
 */
int terrainLightStep(float bearingDegrees);
/** The light azimuth for a step: 315 + 45 * step degrees. */
float terrainLightAzimuth(int step);

} // namespace maps
