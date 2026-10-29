// SPDX-License-Identifier: GPL-3.0-or-later
#include "Raster.h"

#include "../platform/Platform.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace maps {

namespace {

constexpr float PI_F = 3.14159265f;
constexpr int MAX_DOT_SEGMENTS = 32;

/** Integral of the "left of x" fraction for a row crossing that spans left..right horizontally. */
inline float coverageIntegral(float x, float left, float right) {
    if (x <= left) {
        return 0.0f;
    }
    if (x >= right) {
        return 0.5f * (right - left) + (x - right);
    }
    const float t = x - left;
    return t * t / (2.0f * (right - left));
}

} // namespace

uint16_t blendRgb565(uint16_t foreground, uint16_t background, uint32_t alpha32) {
    const uint32_t fg = (foreground | (static_cast<uint32_t>(foreground) << 16)) & 0x07E0F81FU;
    const uint32_t bg = (background | (static_cast<uint32_t>(background) << 16)) & 0x07E0F81FU;
    const uint32_t result = ((((fg - bg) * alpha32) >> 5) + bg) & 0x07E0F81FU;
    return static_cast<uint16_t>(result | (result >> 16));
}

Raster::~Raster() {
    largeFree(cells);
    largeFree(rowFirst);
    largeFree(rowLast);
}

void Raster::resetRows() {
    for (int row = 0; row < rasterHeight; row++) {
        rowFirst[row] = static_cast<int16_t>(stride);
        rowLast[row] = -1;
    }
}

bool Raster::init(int width, int height) {
    if (cells != nullptr && width == rasterWidth && height == rasterHeight) {
        return true;
    }
    largeFree(cells);
    largeFree(rowFirst);
    largeFree(rowLast);
    rasterWidth = width;
    rasterHeight = height;
    stride = width + 2;
    cells = static_cast<float*>(largeAlloc(static_cast<size_t>(stride) * height * sizeof(float)));
    rowFirst = static_cast<int16_t*>(fastAlloc(static_cast<size_t>(height) * sizeof(int16_t)));
    rowLast = static_cast<int16_t*>(fastAlloc(static_cast<size_t>(height) * sizeof(int16_t)));
    if (cells == nullptr || rowFirst == nullptr || rowLast == nullptr) {
        return false;
    }
    memset(cells, 0, static_cast<size_t>(stride) * height * sizeof(float));
    resetRows();
    minRow = rasterHeight;
    maxRow = -1;
    minColumn = stride;
    maxColumn = 0;
    return true;
}

void Raster::markRows(int top, int bottom) {
    minRow = std::min(minRow, top);
    maxRow = std::max(maxRow, bottom);
}

void Raster::addLine(float x0, float y0, float x1, float y1) {
    const auto height = static_cast<float>(rasterHeight);
    if (y0 == y1 || (y0 <= 0.0f && y1 <= 0.0f) || (y0 >= height && y1 >= height)) {
        return;
    }
    if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) {
        return;
    }
    const float slope = (x1 - x0) / (y1 - y0);
    if (y0 < 0.0f) {
        x0 += (0.0f - y0) * slope;
        y0 = 0.0f;
    } else if (y0 > height) {
        x0 += (height - y0) * slope;
        y0 = height;
    }
    if (y1 < 0.0f) {
        x1 += (0.0f - y1) * slope;
        y1 = 0.0f;
    } else if (y1 > height) {
        x1 += (height - y1) * slope;
        y1 = height;
    }
    addClippedLine(x0, y0, x1, y1);
}

void Raster::addClippedLine(float x0, float y0, float x1, float y1) {
    const auto width = static_cast<float>(rasterWidth);
    // Splits the edge where it crosses x = 0 and x = width. Parts left of the raster become vertical edges
    // at x = 0, parts right of it do not affect any visible pixel.
    float splits[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    int count = 1;
    if (x0 != x1) {
        for (const float edge : { 0.0f, width }) {
            const float t = (edge - x0) / (x1 - x0);
            if (t > 0.0f && t < 1.0f) {
                splits[count++] = t;
            }
        }
    }
    splits[count] = 1.0f;
    if (count == 3 && splits[1] > splits[2]) {
        std::swap(splits[1], splits[2]);
    }
    for (int i = 0; i < count; i++) {
        const float ta = splits[i];
        const float tb = splits[i + 1];
        float ax = x0 + (x1 - x0) * ta;
        const float ay = y0 + (y1 - y0) * ta;
        float bx = x0 + (x1 - x0) * tb;
        const float by = y0 + (y1 - y0) * tb;
        const float middle = 0.5f * (ax + bx);
        if (middle >= width) {
            // Rows left of a skipped edge keep their coverage up to the right border
            const int top = std::max(0, static_cast<int>(std::min(ay, by)));
            const int bottom = std::min(rasterHeight - 1, static_cast<int>(std::ceil(std::max(ay, by))) - 1);
            for (int row = top; row <= bottom; row++) {
                rowLast[row] = static_cast<int16_t>(stride - 1);
            }
            maxColumn = stride - 1;
            continue;
        }
        if (middle <= 0.0f) {
            ax = 0.0f;
            bx = 0.0f;
        }
        accumulate(std::clamp(ax, 0.0f, width), ay, std::clamp(bx, 0.0f, width), by);
    }
}

void Raster::accumulateAliased(float x0, float y0, float x1, float y1) {
    float direction = 1.0f;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        direction = -1.0f;
    }
    // The rows whose centre lies inside the edge's span
    const int firstRow = std::max(0, static_cast<int>(std::ceil(y0 - 0.5f)));
    const int lastRow = std::min(rasterHeight - 1, static_cast<int>(std::ceil(y1 - 0.5f)) - 1);
    if (firstRow > lastRow) {
        return;
    }
    markRows(firstRow, lastRow);
    const float slope = (x1 - x0) / (y1 - y0);
    for (int row = firstRow; row <= lastRow; row++) {
        const float x = x0 + (static_cast<float>(row) + 0.5f - y0) * slope;
        // Pixels whose centre is right of the edge are inside
        const int column = std::clamp(static_cast<int>(std::floor(x + 0.5f)), 0, stride - 1);
        cells[row * stride + column] += direction;
        minColumn = std::min(minColumn, column);
        maxColumn = std::max(maxColumn, column);
        if (column < rowFirst[row]) {
            rowFirst[row] = static_cast<int16_t>(column);
        }
        if (column > rowLast[row]) {
            rowLast[row] = static_cast<int16_t>(column);
        }
    }
}

void Raster::accumulate(float x0, float y0, float x1, float y1) {
    if (y0 == y1) {
        return;
    }
    if (!antialias) {
        accumulateAliased(x0, y0, x1, y1);
        return;
    }
    float direction = 1.0f;
    if (y0 > y1) {
        std::swap(x0, x1);
        std::swap(y0, y1);
        direction = -1.0f;
    }
    const float slope = (x1 - x0) / (y1 - y0);
    const int firstRow = static_cast<int>(y0);
    const int lastRow = std::min(rasterHeight - 1, static_cast<int>(std::ceil(y1)) - 1);
    if (firstRow > lastRow) {
        return;
    }
    markRows(firstRow, lastRow);
    for (int row = firstRow; row <= lastRow; row++) {
        const float top = std::max(y0, static_cast<float>(row));
        const float bottom = std::min(y1, static_cast<float>(row + 1));
        const float dy = bottom - top;
        if (dy <= 0.0f) {
            continue;
        }
        const float xa = x0 + (top - y0) * slope;
        const float xb = x0 + (bottom - y0) * slope;
        const float left = std::min(xa, xb);
        const float right = std::max(xa, xb);
        const float area = dy * direction;
        float* line = cells + row * stride;
        const int firstColumn = static_cast<int>(left);
        const int lastColumn = std::min(stride - 1, static_cast<int>(right) + 1);
        minColumn = std::min(minColumn, firstColumn);
        maxColumn = std::max(maxColumn, lastColumn);
        if (firstColumn < rowFirst[row]) {
            rowFirst[row] = static_cast<int16_t>(firstColumn);
        }
        if (lastColumn > rowLast[row]) {
            rowLast[row] = static_cast<int16_t>(lastColumn);
        }
        if (right - left < 1e-4f) {
            const float fraction = 1.0f - (left - static_cast<float>(firstColumn));
            line[firstColumn] += area * fraction;
            if (firstColumn + 1 < stride) {
                line[firstColumn + 1] += area * (1.0f - fraction);
            }
            continue;
        }
        float previous = 0.0f;
        for (int column = firstColumn; column <= lastColumn; column++) {
            const auto c = static_cast<float>(column);
            const float cover = coverageIntegral(c + 1.0f, left, right) - coverageIntegral(c, left, right);
            line[column] += area * (cover - previous);
            previous = cover;
        }
    }
}

void Raster::addSegment(float x0, float y0, float x1, float y1, float halfWidth) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = std::sqrt(dx * dx + dy * dy);
    if (length < 1e-4f) {
        return;
    }
    const float extent = halfWidth + 1.0f;
    const auto width = static_cast<float>(rasterWidth);
    const auto height = static_cast<float>(rasterHeight);
    if ((x0 < -extent && x1 < -extent) || (x0 > width + extent && x1 > width + extent) ||
        (y0 < -extent && y1 < -extent) || (y0 > height + extent && y1 > height + extent)) {
        return;
    }
    const float nx = -dy / length * halfWidth;
    const float ny = dx / length * halfWidth;
    addLine(x0 + nx, y0 + ny, x1 + nx, y1 + ny);
    addLine(x1 + nx, y1 + ny, x1 - nx, y1 - ny);
    addLine(x1 - nx, y1 - ny, x0 - nx, y0 - ny);
    addLine(x0 - nx, y0 - ny, x0 + nx, y0 + ny);
}

void Raster::addDot(float x, float y, float radius) {
    const float extent = radius + 1.0f;
    if (x < -extent || y < -extent || x > static_cast<float>(rasterWidth) + extent || y > static_cast<float>(rasterHeight) + extent) {
        return;
    }
    const int segments = std::clamp(static_cast<int>(radius * 2.0f) + 4, 6, MAX_DOT_SEGMENTS);
    float previousX = x + radius;
    float previousY = y;
    for (int i = 1; i <= segments; i++) {
        // Clockwise in screen coordinates, the same winding as the segment quads
        const float angle = -2.0f * PI_F * static_cast<float>(i) / static_cast<float>(segments);
        const float nextX = x + radius * std::cos(angle);
        const float nextY = y + radius * std::sin(angle);
        addLine(previousX, previousY, nextX, nextY);
        previousX = nextX;
        previousY = nextY;
    }
}

void Raster::fill(uint16_t* pixels, int pixelStride, uint16_t color, uint8_t opacity) {
    if (isEmpty()) {
        return;
    }
    const float scale = static_cast<float>(opacity) * (32.0f / 255.0f);
    for (int row = minRow; row <= maxRow; row++) {
        const int first = rowFirst[row];
        int last = rowLast[row];
        if (last < first) {
            continue;
        }
        // A row whose edges are all left of the raster is covered up to the right border
        float* line = cells + row * stride;
        uint16_t* out = pixels + row * pixelStride;
        float sum = 0.0f;
        for (int column = first; column <= last; column++) {
            sum += line[column];
            line[column] = 0.0f;
            if (column >= rasterWidth) {
                continue;
            }
            const float coverage = std::min(1.0f, std::fabs(sum));
            const auto alpha = static_cast<uint32_t>(coverage * scale + 0.5f);
            if (alpha >= 32) {
                out[column] = color;
            } else if (alpha > 0) {
                out[column] = blendRgb565(color, out[column], alpha);
            }
            if (column == last && last < rasterWidth - 1 && std::fabs(sum) > 1e-3f) {
                last = rasterWidth - 1;
            }
        }
        rowFirst[row] = static_cast<int16_t>(stride);
        rowLast[row] = -1;
    }
    minRow = rasterHeight;
    maxRow = -1;
    minColumn = stride;
    maxColumn = 0;
}

void Raster::clear() {
    if (cells != nullptr && !isEmpty()) {
        for (int row = minRow; row <= maxRow; row++) {
            memset(cells + row * stride, 0, static_cast<size_t>(stride) * sizeof(float));
        }
        resetRows();
    }
    minRow = rasterHeight;
    maxRow = -1;
    minColumn = stride;
    maxColumn = 0;
}

} // namespace maps
