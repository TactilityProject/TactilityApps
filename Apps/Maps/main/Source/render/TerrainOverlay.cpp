// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerrainOverlay.h"

#include "../platform/Platform.h"
#include "Contours.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <new>

namespace maps {

namespace {

constexpr int ROW_BYTES = TERRAIN_TILE_SIZE * 2;
constexpr int MAX_LINE_POINTS = 2048;

uint16_t get16(const uint8_t* bytes) {
    return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
}

constexpr int READ_ROWS = static_cast<int>(TERRAIN_READ_BYTES / ROW_BYTES);

/**
 * Rows of a height file read TERRAIN_READ_BYTES at a time: one card read instead of one per row (a row read with a
 * seek cost 4-14 ms on the T-Deck).
 */
struct RowBlocks {
    FILE* file;
    uint8_t* buffer;
    int first = -1;
    int count = 0;

    const uint8_t* row(int y) {
        if (file == nullptr) {
            return nullptr;
        }
        if (y < first || y >= first + count) {
            const int rows = std::min(READ_ROWS, TERRAIN_TILE_SIZE - y);
            first = -1;
            count = 0;
            if (rows <= 0 || fseek(file, static_cast<long>(TERRAIN_FILE_HEADER + static_cast<size_t>(y) * ROW_BYTES), SEEK_SET) != 0 ||
                fread(buffer, 1, static_cast<size_t>(rows) * ROW_BYTES, file) != static_cast<size_t>(rows) * ROW_BYTES) {
                return nullptr;
            }
            first = y;
            count = rows;
        }
        return buffer + static_cast<size_t>(y - first) * ROW_BYTES;
    }
};

bool headerOk(FILE* file) {
    uint8_t header[TERRAIN_FILE_HEADER];
    return file != nullptr && fseek(file, 0, SEEK_SET) == 0 && fread(header, 1, sizeof(header), file) == sizeof(header) &&
        checkTerrainHeader(header);
}

uint16_t darken(uint16_t color, int keep) {
    // keep is 0..256 of each channel
    const int r = ((color >> 11) & 31) * keep >> 8;
    const int g = ((color >> 5) & 63) * keep >> 8;
    const int b = (color & 31) * keep >> 8;
    return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

} // namespace

void writeTerrainHeader(uint8_t* header) {
    memset(header, 0, TERRAIN_FILE_HEADER);
    memcpy(header, "TMHG", 4);
    header[4] = TERRAIN_FILE_VERSION;
    header[5] = TERRAIN_ZOOM;
    header[6] = static_cast<uint8_t>(TERRAIN_TILE_SIZE & 0xFF);
    header[7] = static_cast<uint8_t>(TERRAIN_TILE_SIZE >> 8);
}

bool terrariumToHeightFile(ByteInput& png, InflateReader& inflater, uint8_t* rowBuffer, uint8_t* file) {
    writeTerrainHeader(file);
    return decodeTerrarium(png, inflater, rowBuffer, reinterpret_cast<uint16_t*>(file + TERRAIN_FILE_HEADER), TERRAIN_TILE_SIZE);
}

bool checkTerrainHeader(const uint8_t* header) {
    return memcmp(header, "TMHG", 4) == 0 && header[4] == TERRAIN_FILE_VERSION && header[5] == TERRAIN_ZOOM && get16(header + 6) == TERRAIN_TILE_SIZE;
}

bool terrainSpan(const TileKey& target, TerrainSpan& span) {
    if (target.z < TERRAIN_ZOOM || target.z > 30) {
        return false;
    }
    const int dz = target.z - TERRAIN_ZOOM;
    span.dem = ancestorOf(target, TERRAIN_ZOOM);
    // File points across the target tile: 256 at z12, 128 at z13, 64 at z14, at least 1 cell deeper
    const int across = dz >= 8 ? 1 : TERRAIN_TILE_SIZE >> dz;
    const int localX = target.x - (span.dem.x << dz);
    const int localY = target.y - (span.dem.y << dz);
    if (dz >= 8) {
        // Several target tiles share one cell: the patch is that cell, drawn magnified
        span.x0 = localX >> (dz - 8);
        span.y0 = localY >> (dz - 8);
    } else {
        span.x0 = localX * across;
        span.y0 = localY * across;
    }
    span.step = across > TERRAIN_PATCH_CELLS ? across / TERRAIN_PATCH_CELLS : 1;
    span.cells = across / span.step;
    return true;
}

void terrainFilesNeeded(const TerrainSpan& span, bool needed[9]) {
    // The patch's points and its margin run from x0 - step to x0 + (cells + 1) * step
    const int low = -span.step;
    const int high = (span.cells + 1) * span.step;
    // The west file only where the patch covers at most a quarter of its rows (z14 and deeper): at z12 its one column
    // of margin cost a read of the whole west file (2 s a tile instead of 0.4 s), for a seam of one pixel
    const bool west = span.x0 + low < 0 && span.cells * span.step <= TERRAIN_TILE_SIZE / 4;
    const bool east = span.x0 + high >= TERRAIN_TILE_SIZE;
    // The same for the north files: a row from each costs an open, tens of milliseconds on a FAT card
    const bool north = span.y0 + low < 0 && span.cells * span.step <= TERRAIN_TILE_SIZE / 4;
    const bool south = span.y0 + high >= TERRAIN_TILE_SIZE;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            const bool column = dx == 0 || (dx < 0 ? west : east);
            const bool row = dy == 0 || (dy < 0 ? north : south);
            needed[(dy + 1) * 3 + dx + 1] = column && row;
        }
    }
}

bool readTerrainPatch(TerrainFileOpener open, void* context, const TerrainSpan& span, uint16_t* grid, uint8_t* rowBuffer) {
    if (span.cells <= 0 || span.cells > TERRAIN_PATCH_CELLS) {
        return false;
    }
    bool needed[9];
    terrainFilesNeeded(span, needed);
    // The row of files the own one is in stays open while its rows are read (west, own, east), in blocks
    FILE* middleFiles[3] = {};
    for (int c = 0; c < 3; c++) {
        if (needed[3 + c]) {
            middleFiles[c] = open(3 + c, context);
            if (middleFiles[c] != nullptr && !headerOk(middleFiles[c])) {
                fclose(middleFiles[c]);
                middleFiles[c] = nullptr;
            }
        }
    }
    auto closeMiddle = [&] {
        for (FILE*& file : middleFiles) {
            if (file != nullptr) {
                fclose(file);
                file = nullptr;
            }
        }
    };
    if (middleFiles[1] == nullptr) {
        closeMiddle();
        return false;
    }
    RowBlocks middle[3] = { { middleFiles[0], rowBuffer + 2 * TERRAIN_READ_BYTES }, { middleFiles[1], rowBuffer },
        { middleFiles[2], rowBuffer + TERRAIN_READ_BYTES } };
    uint8_t* single = rowBuffer + 3 * TERRAIN_READ_BYTES;
    const int last = span.cells + 1;
    bool rowRead[TERRAIN_GRID_STRIDE];
    FILE* edgeFiles[9] = {};
    bool edgeTried[9] = {};
    auto closeEdges = [&] {
        for (FILE*& file : edgeFiles) {
            if (file != nullptr) {
                fclose(file);
                file = nullptr;
            }
        }
    };
    for (int j = -1; j <= last; j++) {
        const int y = span.y0 + j * span.step;
        const int fy = y < 0 ? 0 : (y >= TERRAIN_TILE_SIZE ? 2 : 1);
        const int ry = y - (fy - 1) * TERRAIN_TILE_SIZE;
        if (fy == 2) {
            // The rows south of the file come last: the middle files close first (the card allows four open files)
            closeMiddle();
        }
        const uint8_t* rows[3] = {};
        for (int c = 0; c < 3; c++) {
            if (ry < 0 || ry >= TERRAIN_TILE_SIZE) {
                continue;
            }
            if (fy == 1) {
                rows[c] = middle[c].file != nullptr ? middle[c].row(ry) : nullptr;
                continue;
            }
            const int index = fy * 3 + c;
            if (!needed[index]) {
                continue;
            }
            // A row north or south of the file: each of those files is opened once (the south ones give two rows at
            // z12) and closed at the end, the middle ones are closed by then
            if (!edgeTried[index]) {
                edgeTried[index] = true;
                edgeFiles[index] = open(index, context);
                if (edgeFiles[index] != nullptr && !headerOk(edgeFiles[index])) {
                    fclose(edgeFiles[index]);
                    edgeFiles[index] = nullptr;
                }
            }
            FILE* file = edgeFiles[index];
            if (file == nullptr) {
                continue;
            }
            uint8_t* into = single + static_cast<size_t>(c) * ROW_BYTES;
            if (fseek(file, static_cast<long>(TERRAIN_FILE_HEADER + static_cast<size_t>(ry) * ROW_BYTES), SEEK_SET) == 0 &&
                fread(into, 1, ROW_BYTES, file) == static_cast<size_t>(ROW_BYTES)) {
                rows[c] = into;
            }
            if (fy == 0) {
                // The north row is the first and only one from its file
                fclose(file);
                edgeFiles[index] = nullptr;
            }
        }
        if (fy == 1 && rows[1] == nullptr) {
            closeMiddle();
            closeEdges();
            return false;
        }
        uint16_t* out = grid + static_cast<size_t>(j + 1) * TERRAIN_GRID_STRIDE;
        // Points whose file is not on the card take the nearest point of the row that is
        int firstRead = -1;
        int lastRead = -1;
        for (int i = -1; i <= last; i++) {
            const int x = span.x0 + i * span.step;
            const int fx = x < 0 ? 0 : (x >= TERRAIN_TILE_SIZE ? 2 : 1);
            const int rx = x - (fx - 1) * TERRAIN_TILE_SIZE;
            const uint8_t* row = rows[fx];
            if (row == nullptr || rx < 0 || rx >= TERRAIN_TILE_SIZE) {
                continue;
            }
            out[i + 1] = get16(row + rx * 2);
            if (firstRead < 0) {
                firstRead = i + 1;
            } else {
                // A gap inside the row (a middle file north or south missing): the point before it
                for (int g = lastRead + 1; g < i + 1; g++) {
                    out[g] = out[lastRead];
                }
            }
            lastRead = i + 1;
        }
        rowRead[j + 1] = firstRead >= 0;
        if (firstRead < 0) {
            continue;
        }
        for (int g = 0; g < firstRead; g++) {
            out[g] = out[firstRead];
        }
        for (int g = lastRead + 1; g <= last + 1; g++) {
            out[g] = out[lastRead];
        }
    }
    closeMiddle();
    closeEdges();
    // Rows of files not on the card: the nearest row read (the own file's rows always are)
    const size_t rowBytes = static_cast<size_t>(last + 2) * 2;
    for (int j = 1; j <= last + 1; j++) {
        if (!rowRead[j] && rowRead[j - 1]) {
            memcpy(grid + static_cast<size_t>(j) * TERRAIN_GRID_STRIDE, grid + static_cast<size_t>(j - 1) * TERRAIN_GRID_STRIDE, rowBytes);
            rowRead[j] = true;
        }
    }
    for (int j = last; j >= 0; j--) {
        if (!rowRead[j] && rowRead[j + 1]) {
            memcpy(grid + static_cast<size_t>(j) * TERRAIN_GRID_STRIDE, grid + static_cast<size_t>(j + 1) * TERRAIN_GRID_STRIDE, rowBytes);
            rowRead[j] = true;
        }
    }
    return true;
}

bool readTerrainHeight(FILE* file, double x, double y, float& metres) {
    if (!headerOk(file) || !(x >= 0.0 && x <= TERRAIN_TILE_SIZE && y >= 0.0 && y <= TERRAIN_TILE_SIZE)) {
        return false;
    }
    // The east and south edge use the file's last points
    const int x0 = std::min(static_cast<int>(x), TERRAIN_TILE_SIZE - 2);
    const int y0 = std::min(static_cast<int>(y), TERRAIN_TILE_SIZE - 2);
    uint8_t rows[2][4];
    for (int r = 0; r < 2; r++) {
        if (fseek(file, static_cast<long>(TERRAIN_FILE_HEADER + static_cast<size_t>(y0 + r) * ROW_BYTES + static_cast<size_t>(x0) * 2), SEEK_SET) != 0 ||
            fread(rows[r], 1, 4, file) != 4) {
            return false;
        }
    }
    const double fx = std::min(1.0, x - x0);
    const double fy = std::min(1.0, y - y0);
    const double top = heightFromCode(get16(rows[0])) * (1.0 - fx) + heightFromCode(get16(rows[0] + 2)) * fx;
    const double bottom = heightFromCode(get16(rows[1])) * (1.0 - fx) + heightFromCode(get16(rows[1] + 2)) * fx;
    metres = static_cast<float>(top * (1.0 - fy) + bottom * fy);
    return true;
}

float terrainHeightIn(const uint8_t* file, double x, double y) {
    x = std::clamp(x, 0.0, static_cast<double>(TERRAIN_TILE_SIZE));
    y = std::clamp(y, 0.0, static_cast<double>(TERRAIN_TILE_SIZE));
    const int x0 = std::min(static_cast<int>(x), TERRAIN_TILE_SIZE - 2);
    const int y0 = std::min(static_cast<int>(y), TERRAIN_TILE_SIZE - 2);
    const uint8_t* row0 = file + TERRAIN_FILE_HEADER + static_cast<size_t>(y0) * ROW_BYTES + static_cast<size_t>(x0) * 2;
    const uint8_t* row1 = row0 + ROW_BYTES;
    const double fx = std::min(1.0, x - x0);
    const double fy = std::min(1.0, y - y0);
    const double top = heightFromCode(get16(row0)) * (1.0 - fx) + heightFromCode(get16(row0 + 2)) * fx;
    const double bottom = heightFromCode(get16(row1)) * (1.0 - fx) + heightFromCode(get16(row1 + 2)) * fx;
    return static_cast<float>(top * (1.0 - fy) + bottom * fy);
}

float terrainCellMetres(const TileKey& target, const TerrainSpan& span) {
    const double worldY = (static_cast<double>(target.y) + 0.5) * TILE_SIZE;
    const double lat = worldYToLat(worldY, target.z);
    // One file point is one z12 pixel
    const double cell = metresPerPixel(lat, TERRAIN_ZOOM) * span.step;
    const int dz = target.z - TERRAIN_ZOOM;
    return static_cast<float>(dz >= 8 ? metresPerPixel(lat, TERRAIN_ZOOM) : cell);
}

void contourIntervals(int zoom, float& minorM, float& majorM) {
    if (zoom <= 12) {
        minorM = 100.0f;
        majorM = 500.0f;
    } else if (zoom == 13) {
        minorM = 50.0f;
        majorM = 250.0f;
    } else if (zoom <= 15) {
        minorM = 20.0f;
        majorM = 100.0f;
    } else {
        minorM = 10.0f;
        majorM = 50.0f;
    }
}

TerrainOverlay::~TerrainOverlay() {
    end();
}

bool TerrainOverlay::begin() {
    if (grid != nullptr) {
        return true;
    }
    grid = static_cast<uint16_t*>(largeAlloc(static_cast<size_t>(TERRAIN_GRID_STRIDE) * TERRAIN_GRID_STRIDE * 2));
    shade = static_cast<uint8_t*>(largeAlloc(static_cast<size_t>(TERRAIN_GRID_STRIDE) * TERRAIN_GRID_STRIDE));
    row = static_cast<uint8_t*>(largeAlloc(TERRAIN_ROW_BUFFER_BYTES));
    simplified = static_cast<float*>(largeAlloc(static_cast<size_t>(MAX_LINE_POINTS) * 2 * sizeof(float)));
    void* place = largeAlloc(sizeof(ContourTracer));
    tracer = place != nullptr ? new (place) ContourTracer() : nullptr;
    if (grid == nullptr || shade == nullptr || row == nullptr || simplified == nullptr || tracer == nullptr ||
        !tracer->begin(TERRAIN_PATCH_POINTS, TERRAIN_PATCH_POINTS, MAX_LINE_POINTS)) {
        end();
        return false;
    }
    return true;
}

void TerrainOverlay::end() {
    if (tracer != nullptr) {
        tracer->~ContourTracer();
        largeFree(tracer);
        tracer = nullptr;
    }
    largeFree(grid);
    largeFree(shade);
    largeFree(row);
    largeFree(simplified);
    grid = nullptr;
    shade = nullptr;
    row = nullptr;
    simplified = nullptr;
    patchCells = 0;
}

void TerrainOverlay::draw(uint16_t* pixels, int zoom, const Palette& palette, Raster& raster) {
    lines = 0;
    if (grid == nullptr || patchCells <= 0 || palette.binary) {
        return;
    }
    // Monochrome draws only the lines: a shade would turn into grey patches
    if (&palette != &monochromePalette()) {
        shadePixels(pixels, palette);
    }
    drawContours(zoom, palette, raster, pixels);
}

void TerrainOverlay::shadePixels(uint16_t* pixels, const Palette& palette) {
    (void)palette;
    const int points = patchCells + 1;
    // A slope reads better a little steeper on a small screen, more so when the cells are large
    const float exaggeration = metresPerCell > 30.0f ? 2.0f : 1.5f;
    // Over the patch and its margin: the patch's own edge points get their slope from both sides
    hillshade(grid, points + 2, points + 2, TERRAIN_GRID_STRIDE, metresPerCell, exaggeration, shade, TERRAIN_GRID_STRIDE,
        terrainLightAzimuth(lightStep));
    // Bilinear over the shade points, in 16.16: pixel centre (p + 0.5) / 256 of the patch
    const int32_t stepFixed = (patchCells << 16) / TILE_SIZE;
    for (int py = 0; py < TILE_SIZE; py++) {
        const int32_t gy = static_cast<int32_t>((static_cast<int64_t>(py) * 2 + 1) * stepFixed / 2);
        int y0 = gy >> 16;
        if (y0 >= patchCells) {
            y0 = patchCells - 1;
        }
        const int fy = (gy - (y0 << 16)) >> 8;
        const uint8_t* rowA = shade + static_cast<size_t>(y0 + 1) * TERRAIN_GRID_STRIDE + 1;
        const uint8_t* rowB = rowA + TERRAIN_GRID_STRIDE;
        uint16_t* out = pixels + static_cast<size_t>(py) * TILE_SIZE;
        for (int px = 0; px < TILE_SIZE; px++) {
            const int32_t gx = static_cast<int32_t>((static_cast<int64_t>(px) * 2 + 1) * stepFixed / 2);
            int x0 = gx >> 16;
            if (x0 >= patchCells) {
                x0 = patchCells - 1;
            }
            const int fx = (gx - (x0 << 16)) >> 8;
            const int top = rowA[x0] * (256 - fx) + rowA[x0 + 1] * fx;
            const int bottom = rowB[x0] * (256 - fx) + rowB[x0 + 1] * fx;
            const int value = (top * (256 - fy) + bottom * fy) >> 16;
            if (value > 2) {
                // At most 45 % darker
                out[px] = darken(out[px], 256 - value * 115 / 255);
            }
        }
    }
}

namespace {

/** Digits 0-9, 5 x 7 pixels, a row a byte (bit 4 is the left column): contour heights need no font. */
constexpr uint8_t DIGITS[10][7] = {
    { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
    { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
    { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
    { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
};
constexpr int DIGIT_WIDTH = 5;
constexpr int DIGIT_HEIGHT = 7;
/** Labels keep this far from each other and from the tile's edge, where the next tile may put its own. */
constexpr float LABEL_SPACING = 60.0f;
constexpr float LABEL_EDGE = 14.0f;
/** A major line shorter than this (tile pixels) gets no number. */
constexpr float LABEL_MIN_LINE = 50.0f;

} // namespace

void TerrainOverlay::drawLabels(uint16_t* pixels, uint16_t ink, uint16_t halo) {
    for (int l = 0; l < labelCount; l++) {
        char text[8];
        const int length = snprintf(text, sizeof(text), "%d", contourLabels[l].metres);
        const int width = length * (DIGIT_WIDTH + 1) - 1;
        const int left = static_cast<int>(contourLabels[l].x) - width / 2;
        const int top = static_cast<int>(contourLabels[l].y) - DIGIT_HEIGHT / 2;
        // A halo first (every pixel next to a digit's), then the digits over it
        for (int pass = 0; pass < 2; pass++) {
            for (int c = 0; c < length; c++) {
                if (text[c] < '0' || text[c] > '9') {
                    continue;
                }
                const uint8_t* rows = DIGITS[text[c] - '0'];
                for (int row = 0; row < DIGIT_HEIGHT; row++) {
                    for (int column = 0; column < DIGIT_WIDTH; column++) {
                        if ((rows[row] & (0x10 >> column)) == 0) {
                            continue;
                        }
                        const int x = left + c * (DIGIT_WIDTH + 1) + column;
                        const int y = top + row;
                        for (int dy = pass == 0 ? -1 : 0; dy <= (pass == 0 ? 1 : 0); dy++) {
                            for (int dx = pass == 0 ? -1 : 0; dx <= (pass == 0 ? 1 : 0); dx++) {
                                const int px = x + dx;
                                const int py = y + dy;
                                if (px >= 0 && py >= 0 && px < TILE_SIZE && py < TILE_SIZE) {
                                    pixels[static_cast<size_t>(py) * TILE_SIZE + px] = pass == 0 ? halo : ink;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}

void TerrainOverlay::onLine(void* context, float levelM, const float* points, int pointCount, bool closed) {
    auto* self = static_cast<TerrainOverlay*>(context);
    const float rest = std::fmod(levelM, self->passMajorM);
    const bool major = std::fabs(rest) < 0.01f || std::fabs(rest - self->passMajorM) < 0.01f || std::fabs(rest + self->passMajorM) < 0.01f;
    if (major != self->passMajor || pointCount < 2) {
        return;
    }
    const int count = pointCount < MAX_LINE_POINTS ? pointCount : MAX_LINE_POINTS;
    memcpy(self->simplified, points, static_cast<size_t>(count) * 2 * sizeof(float));
    // Simplified to a third of a pixel
    const float tolerance = 0.33f / self->passScale;
    const int kept = simplifyLine(self->simplified, count, tolerance);
    Raster& raster = *self->passRaster;
    const float scale = self->passScale;
    for (int i = 1; i < kept; i++) {
        raster.addSegment(self->simplified[i * 2 - 2] * scale, self->simplified[i * 2 - 1] * scale, self->simplified[i * 2] * scale,
            self->simplified[i * 2 + 1] * scale, self->passHalfWidth);
    }
    if (closed && kept > 2) {
        raster.addSegment(self->simplified[kept * 2 - 2] * scale, self->simplified[kept * 2 - 1] * scale, self->simplified[0] * scale,
            self->simplified[1] * scale, self->passHalfWidth);
    }
    self->lines++;
    if (self->passLabels && self->labelCount < MAX_CONTOUR_LABELS) {
        // The point half way along a long enough line, away from the tile's edge and from the other numbers
        float total = 0.0f;
        for (int i = 1; i < kept; i++) {
            const float dx = (self->simplified[i * 2] - self->simplified[i * 2 - 2]) * scale;
            const float dy = (self->simplified[i * 2 + 1] - self->simplified[i * 2 - 1]) * scale;
            total += std::sqrt(dx * dx + dy * dy);
        }
        if (total >= LABEL_MIN_LINE) {
            float walked = 0.0f;
            for (int i = 1; i < kept; i++) {
                const float ax = self->simplified[i * 2 - 2] * scale;
                const float ay = self->simplified[i * 2 - 1] * scale;
                const float bx = self->simplified[i * 2] * scale;
                const float by = self->simplified[i * 2 + 1] * scale;
                const float step = std::sqrt((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
                if (walked + step >= total / 2.0f && step > 0.0f) {
                    const float t = (total / 2.0f - walked) / step;
                    const float x = ax + (bx - ax) * t;
                    const float y = ay + (by - ay) * t;
                    bool free = x >= LABEL_EDGE && y >= LABEL_EDGE && x <= TILE_SIZE - LABEL_EDGE && y <= TILE_SIZE - LABEL_EDGE;
                    for (int l = 0; l < self->labelCount && free; l++) {
                        const float lx = self->contourLabels[l].x - x;
                        const float ly = self->contourLabels[l].y - y;
                        free = lx * lx + ly * ly >= LABEL_SPACING * LABEL_SPACING;
                    }
                    if (free) {
                        self->contourLabels[self->labelCount++] = { x, y, static_cast<int>(std::floor(levelM + 0.5f)) };
                    }
                    break;
                }
                walked += step;
            }
        }
    }
}

void TerrainOverlay::drawContours(int zoom, const Palette& palette, Raster& raster, uint16_t* pixels) {
    float minorM;
    float majorM;
    contourIntervals(zoom, minorM, majorM);
    const bool mono = &palette == &monochromePalette();
    const int points = patchCells + 1;
    passRaster = &raster;
    passScale = static_cast<float>(TILE_SIZE) / static_cast<float>(patchCells);
    passMajorM = majorM;
    labelCount = 0;
    raster.setAntialias(true);
    for (int pass = 0; pass < 2; pass++) {
        passMajor = pass == 1;
        passHalfWidth = passMajor ? 0.6f : 0.35f;
        // Numbers on the major lines from zoom 13, where they are far enough apart
        passLabels = passMajor && zoom >= 13;
        raster.clear();
        tracer->trace(grid + TERRAIN_GRID_STRIDE + 1, points, points, TERRAIN_GRID_STRIDE, minorM, onLine, this);
        if (raster.isEmpty()) {
            continue;
        }
        uint16_t color;
        uint8_t opacity;
        if (mono) {
            color = 0x0000;
            opacity = passMajor ? 110 : 60;
        } else {
            // Mauve, the major lines darker: apart from the brown paths and tracks and the marked trails' red, blue,
            // yellow and green
            color = passMajor ? 0x7AEE : 0xAC54;
            opacity = passMajor ? 190 : 130;
        }
        raster.fill(pixels, TILE_SIZE, color, opacity);
    }
    raster.clear();
    if (labelCount > 0) {
        // Over the lines, with a pale halo so they read on shading and land cover
        drawLabels(pixels, mono ? static_cast<uint16_t>(0x0000) : static_cast<uint16_t>(0x6A4C), mono ? static_cast<uint16_t>(0xFFFF) : static_cast<uint16_t>(0xF7BE));
    }
}

} // namespace maps
