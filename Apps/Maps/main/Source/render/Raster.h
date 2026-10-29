// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>

namespace maps {

/**
 * Anti-aliased rasteriser with exact area coverage. Edges add signed area deltas to an accumulation buffer,
 * and fill() turns the running sum of each row into coverage, so one shape made of many polygons, quads and
 * dots is blended only once. Coverage is min(1, |winding|), which gives nonzero filling.
 * The buffer is allocated once in init(), shapes allocate nothing.
 */
class Raster {
public:
    Raster() = default;
    ~Raster();
    Raster(const Raster&) = delete;
    Raster& operator=(const Raster&) = delete;

    bool init(int width, int height);

    void addLine(float x0, float y0, float x1, float y1);

    /** A stroke segment as a quad, wound like the dots so overlaps never cancel. */
    void addSegment(float x0, float y0, float x1, float y1, float halfWidth);
    /** A round join or cap. */
    void addDot(float x, float y, float radius);

    /**
     * Without anti-aliasing each edge adds one step per row, at the pixel centres: cheaper, for many small
     * shapes where the soft edge does not show. Change it only while the buffer is empty.
     */
    void setAntialias(bool enabled) { antialias = enabled; }

    /** Blends the accumulated shape into pixels with the given colour and clears the buffer. */
    void fill(uint16_t* pixels, int stride, uint16_t color, uint8_t opacity);
    void clear();

    bool isEmpty() const { return minRow > maxRow; }
    int width() const { return rasterWidth; }
    int height() const { return rasterHeight; }

private:
    void addClippedLine(float x0, float y0, float x1, float y1);
    void accumulate(float x0, float y0, float x1, float y1);
    void accumulateAliased(float x0, float y0, float x1, float y1);
    void markRows(int top, int bottom);
    void resetRows();

    float* cells = nullptr;
    /** First and last touched column of each row, so fill() skips the untouched part of a row. */
    int16_t* rowFirst = nullptr;
    int16_t* rowLast = nullptr;
    int rasterWidth = 0;
    int rasterHeight = 0;
    int stride = 0;
    int minRow = 0;
    int maxRow = -1;
    int minColumn = 0;
    int maxColumn = 0;
    bool antialias = true;
};

uint16_t blendRgb565(uint16_t foreground, uint16_t background, uint32_t alpha32);

} // namespace maps
