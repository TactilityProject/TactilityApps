// SPDX-License-Identifier: GPL-3.0-or-later
#include "Compose.h"

#include <cmath>
#include <cstring>

namespace maps {

int composeView(uint16_t* view, int width, int height, double centerX, double centerY, int zoom, TileLookup lookup, void* context, uint16_t placeholder) {
    const auto left = static_cast<int64_t>(std::floor(centerX - width / 2.0));
    const auto top = static_cast<int64_t>(std::floor(centerY - height / 2.0));
    const int64_t worldSize = static_cast<int64_t>(TILE_SIZE) << zoom;
    const auto firstTileX = static_cast<int>(std::floor(static_cast<double>(left) / TILE_SIZE));
    const auto firstTileY = static_cast<int>(std::floor(static_cast<double>(top) / TILE_SIZE));
    const auto lastTileX = static_cast<int>(std::floor(static_cast<double>(left + width - 1) / TILE_SIZE));
    const auto lastTileY = static_cast<int>(std::floor(static_cast<double>(top + height - 1) / TILE_SIZE));
    int missing = 0;
    for (int tileY = firstTileY; tileY <= lastTileY; tileY++) {
        for (int tileX = firstTileX; tileX <= lastTileX; tileX++) {
            const int64_t tileLeft = static_cast<int64_t>(tileX) * TILE_SIZE;
            const int64_t tileTop = static_cast<int64_t>(tileY) * TILE_SIZE;
            const int x0 = static_cast<int>(std::max<int64_t>(tileLeft, left) - left);
            const int x1 = static_cast<int>(std::min<int64_t>(tileLeft + TILE_SIZE, left + width) - left);
            const int y0 = static_cast<int>(std::max<int64_t>(tileTop, top) - top);
            const int y1 = static_cast<int>(std::min<int64_t>(tileTop + TILE_SIZE, top + height) - top);
            const uint16_t* pixels = nullptr;
            if (tileTop >= 0 && tileTop < worldSize) {
                pixels = lookup({ zoom, wrapTileX(tileX, zoom), tileY }, context);
            }
            if (pixels == nullptr) {
                missing++;
                for (int y = y0; y < y1; y++) {
                    uint16_t* row = view + y * width;
                    for (int x = x0; x < x1; x++) {
                        row[x] = placeholder;
                    }
                }
                continue;
            }
            const int sourceX = static_cast<int>(left + x0 - tileLeft);
            for (int y = y0; y < y1; y++) {
                const int sourceY = static_cast<int>(top + y - tileTop);
                memcpy(view + y * width + x0, pixels + sourceY * TILE_SIZE + sourceX, static_cast<size_t>(x1 - x0) * sizeof(uint16_t));
            }
        }
    }
    return missing;
}

void scaleView(const uint16_t* source, uint16_t* target, int width, int height, float scale, float fromX, float fromY, float toX, float toY,
    uint16_t fill) {
    if (scale <= 0.0f) {
        return;
    }
    // Source column of each target column, worked out once per frame
    int columns[1024];
    const int columnCount = width < 1024 ? width : 1024;
    for (int x = 0; x < columnCount; x++) {
        columns[x] = static_cast<int>(std::floor(fromX + (static_cast<float>(x) + 0.5f - toX) / scale));
    }
    for (int y = 0; y < height; y++) {
        const int sourceY = static_cast<int>(std::floor(fromY + (static_cast<float>(y) + 0.5f - toY) / scale));
        uint16_t* out = target + y * width;
        if (sourceY < 0 || sourceY >= height) {
            for (int x = 0; x < width; x++) {
                out[x] = fill;
            }
            continue;
        }
        const uint16_t* row = source + sourceY * width;
        for (int x = 0; x < columnCount; x++) {
            const int sourceX = columns[x];
            out[x] = sourceX >= 0 && sourceX < width ? row[sourceX] : fill;
        }
    }
}

int snapZoom(int zoom, float scale, int maxZoom) {
    if (scale <= 0.0f) {
        return zoom;
    }
    // The nearest power of two, without log2f, which Tactility does not export to apps
    int steps = 0;
    while (scale >= 1.41421356f) {
        scale *= 0.5f;
        steps++;
    }
    while (scale < 0.70710678f) {
        scale *= 2.0f;
        steps--;
    }
    const int snapped = zoom + steps;
    return snapped < 0 ? 0 : (snapped > maxZoom ? maxZoom : snapped);
}

} // namespace maps
