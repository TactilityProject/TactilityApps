// SPDX-License-Identifier: GPL-3.0-or-later
#include "Compose.h"

#include <algorithm>
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

AffineView scaledView(double worldX, double worldY, int zoom, float scale, float anchorX, float anchorY, int width, int height,
    float cosine, float sine) {
    const float inverse = 1.0f / scale;
    // The screen centre lies (width / 2 - anchorX, height / 2 - anchorY) screen pixels from the anchor
    const double offsetX = width / 2.0 - anchorX;
    const double offsetY = height / 2.0 - anchorY;
    const double centerX = worldX + (cosine * offsetX - sine * offsetY) * inverse;
    const double centerY = worldY + (sine * offsetX + cosine * offsetY) * inverse;
    return { centerX, centerY, zoom, cosine * inverse, -sine * inverse, sine * inverse, cosine * inverse };
}

namespace {

constexpr int FALLBACK_LEVELS = 4;

void resolveCell(AffineScratch::Cell& cell, int tileX, int tileY, int zoom, TileLookup lookup, void* context) {
    cell = {};
    const int tileCount = 1 << zoom;
    if (tileY < 0 || tileY >= tileCount) {
        return;
    }
    const int x = wrapTileX(tileX, zoom);
    if ((cell.pixels = lookup({ zoom, x, tileY }, context)) != nullptr) {
        return;
    }
    if (zoom < MAX_VIEW_ZOOM) {
        for (int i = 0; i < 4; i++) {
            cell.children[i] = lookup({ zoom + 1, x * 2 + (i & 1), tileY * 2 + (i >> 1) }, context);
            cell.hasChildren = cell.hasChildren || cell.children[i] != nullptr;
        }
        if (cell.hasChildren) {
            return;
        }
    }
    for (int level = 1; level <= FALLBACK_LEVELS && zoom - level >= 0; level++) {
        const TileKey parent = ancestorOf({ zoom, x, tileY }, zoom - level);
        if ((cell.pixels = lookup(parent, context)) != nullptr) {
            const int size = TILE_SIZE >> level;
            cell.shift = static_cast<uint8_t>(level);
            cell.originX = static_cast<int16_t>((x - (parent.x << level)) * size);
            cell.originY = static_cast<int16_t>((tileY - (parent.y << level)) * size);
            return;
        }
    }
}

inline uint16_t sampleCell(const AffineScratch::Cell& cell, int localX, int localY, uint16_t placeholder) {
    if (cell.pixels != nullptr) {
        return cell.pixels[(cell.originY + (localY >> cell.shift)) * TILE_SIZE + cell.originX + (localX >> cell.shift)];
    }
    if (cell.hasChildren) {
        const uint16_t* child = cell.children[(localY >> 7) * 2 + (localX >> 7)];
        return child != nullptr ? child[((localY & 127) << 1) * TILE_SIZE + ((localX & 127) << 1)] : placeholder;
    }
    return placeholder;
}

} // namespace

int composeAffine(uint16_t* view, int width, int height, const AffineView& transform, TileLookup lookup, void* context, uint16_t placeholder,
    AffineScratch& scratch) {
    // The tiles under the four screen corners bound the ones the view needs
    const float halfWidth = width / 2.0f;
    const float halfHeight = height / 2.0f;
    double minX = 1e300;
    double minY = 1e300;
    double maxX = -1e300;
    double maxY = -1e300;
    for (int corner = 0; corner < 4; corner++) {
        const float sx = (corner & 1) != 0 ? halfWidth : -halfWidth;
        const float sy = (corner & 2) != 0 ? halfHeight : -halfHeight;
        const double wx = transform.centerX + transform.a * sx + transform.b * sy;
        const double wy = transform.centerY + transform.c * sx + transform.d * sy;
        minX = std::min(minX, wx);
        minY = std::min(minY, wy);
        maxX = std::max(maxX, wx);
        maxY = std::max(maxY, wy);
    }
    constexpr int maxGrid = AffineScratch::MAX_GRID;
    const int gridX0 = static_cast<int>(std::floor(minX / TILE_SIZE));
    const int gridY0 = static_cast<int>(std::floor(minY / TILE_SIZE));
    const int gridWidth = std::min(maxGrid, static_cast<int>(std::floor(maxX / TILE_SIZE)) - gridX0 + 1);
    const int gridHeight = std::min(maxGrid, static_cast<int>(std::floor(maxY / TILE_SIZE)) - gridY0 + 1);
    AffineScratch::Cell* grid = scratch.cells;
    int missing = 0;
    for (int y = 0; y < gridHeight; y++) {
        for (int x = 0; x < gridWidth; x++) {
            AffineScratch::Cell& cell = grid[y * gridWidth + x];
            resolveCell(cell, gridX0 + x, gridY0 + y, transform.zoom, lookup, context);
            if (cell.pixels == nullptr && !cell.hasChildren) {
                missing++;
            }
        }
    }

    // 16.16 fixed point from the grid's top left corner: 12 tiles are 3072 pixels, well inside 32 bits
    const double gridLeft = static_cast<double>(gridX0) * TILE_SIZE;
    const double gridTop = static_cast<double>(gridY0) * TILE_SIZE;
    const int32_t limitX = gridWidth * TILE_SIZE;
    const int32_t limitY = gridHeight * TILE_SIZE;
    // lround is not exported to apps
    const auto stepXx = static_cast<int32_t>(std::floor(transform.a * 65536.0 + 0.5));
    const auto stepXy = static_cast<int32_t>(std::floor(transform.c * 65536.0 + 0.5));

    if (transform.b == 0.0f && transform.c == 0.0f && width <= AffineScratch::MAX_WIDTH) {
        // Scale only, the zoom animations and pinch: the source column of each screen column is the same on every
        // row, worked out once, and each row is copied in runs that stay in one tile
        int16_t* columns = scratch.columns;
        const double firstX = transform.centerX + transform.a * (0.5f - halfWidth) - gridLeft;
        auto fx = static_cast<int32_t>(std::floor(firstX * 65536.0));
        for (int x = 0; x < width; x++, fx += stepXx) {
            const int32_t px = fx >> 16;
            columns[x] = static_cast<int16_t>(px >= 0 && px < limitX ? px : -1);
        }
        for (int y = 0; y < height; y++) {
            const double rowY = transform.centerY + transform.d * (static_cast<float>(y) + 0.5f - halfHeight) - gridTop;
            const auto py = static_cast<int32_t>(std::floor(rowY));
            uint16_t* out = view + y * width;
            if (py < 0 || py >= limitY) {
                for (int x = 0; x < width; x++) {
                    out[x] = placeholder;
                }
                continue;
            }
            const AffineScratch::Cell* cells = grid + (py >> 8) * gridWidth;
            const int localY = py & (TILE_SIZE - 1);
            int x = 0;
            while (x < width) {
                const int column = columns[x];
                if (column < 0) {
                    out[x++] = placeholder;
                    continue;
                }
                const int cellX = column >> 8;
                const AffineScratch::Cell& cell = cells[cellX];
                if (cell.pixels != nullptr) {
                    const uint16_t* source = cell.pixels + (cell.originY + (localY >> cell.shift)) * TILE_SIZE + cell.originX;
                    const int shift = cell.shift;
                    // The run ends where the next tile begins
                    for (; x < width && columns[x] >= 0 && (columns[x] >> 8) == cellX; x++) {
                        out[x] = source[(columns[x] & (TILE_SIZE - 1)) >> shift];
                    }
                } else {
                    for (; x < width && columns[x] >= 0 && (columns[x] >> 8) == cellX; x++) {
                        out[x] = sampleCell(cell, columns[x] & (TILE_SIZE - 1), localY, placeholder);
                    }
                }
            }
        }
        return missing;
    }

    for (int y = 0; y < height; y++) {
        const float sy = static_cast<float>(y) + 0.5f - halfHeight;
        const float sx = 0.5f - halfWidth;
        const double rowX = transform.centerX + transform.a * sx + transform.b * sy - gridLeft;
        const double rowY = transform.centerY + transform.c * sx + transform.d * sy - gridTop;
        auto fx = static_cast<int32_t>(std::floor(rowX * 65536.0));
        auto fy = static_cast<int32_t>(std::floor(rowY * 65536.0));
        uint16_t* out = view + y * width;
        for (int x = 0; x < width; x++, fx += stepXx, fy += stepXy) {
            const int32_t px = fx >> 16;
            const int32_t py = fy >> 16;
            if (px < 0 || py < 0 || px >= limitX || py >= limitY) {
                out[x] = placeholder;
                continue;
            }
            out[x] = sampleCell(grid[(py >> 8) * gridWidth + (px >> 8)], px & (TILE_SIZE - 1), py & (TILE_SIZE - 1), placeholder);
        }
    }
    return missing;
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
