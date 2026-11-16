// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>

namespace maps {

/**
 * Contour lines of a height grid (codes as in Terrain.h), traced cell by cell (marching squares) and joined into
 * lines: open ones from the grid's border to its border, closed ones around a top or a hollow. A saddle cell is
 * decided by its centre (the mean of its corners). Points are in grid pixels (x across, y down).
 */
class ContourTracer {
public:
    /** Called once per finished line: its level in metres, its points (x, y pairs) and whether it closes. */
    using LineSink = void (*)(void* context, float levelM, const float* points, int pointCount, bool closed);

    ContourTracer() = default;
    ~ContourTracer();
    ContourTracer(const ContourTracer&) = delete;
    ContourTracer& operator=(const ContourTracer&) = delete;

    /** Working memory for grids up to this size: two bits per cell edge, and a line of up to maxPoints points. */
    bool begin(int maxWidth, int maxHeight, int maxPoints);
    void end();

    /**
     * Traces every level that is a multiple of intervalM between the grid's lowest and highest point. Lines of
     * more than maxPoints points are handed over in pieces (each piece starts where the last ended).
     */
    bool trace(const uint16_t* grid, int width, int height, int stride, float intervalM, LineSink sink, void* context);

    /** Lines handed over in the last trace(), for tests and logs. */
    int lineCount() const { return lines; }

private:
    void traceLevel(const uint16_t* grid, int width, int height, int stride, uint16_t level, float levelM, LineSink sink, void* context);
    void follow(const uint16_t* grid, int width, int height, int stride, uint16_t level, float levelM, int cellX, int cellY, int entrySide,
        LineSink sink, void* context);

    uint8_t* visited = nullptr;
    size_t visitedBytes = 0;
    float* points = nullptr;
    int maxPoints = 0;
    int capacityWidth = 0;
    int capacityHeight = 0;
    int lines = 0;
};

/** Removes points closer than tolerance to the line between their neighbours (Douglas-Peucker); returns the count left. */
int simplifyLine(float* points, int pointCount, float tolerance);

} // namespace maps
