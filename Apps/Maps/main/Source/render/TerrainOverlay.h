// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "../tiles/TileMath.h"
#include "Raster.h"
#include "Style.h"
#include "Terrain.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>

namespace maps {

class ContourTracer;

/**
 * Elevation on the map: hillshade and contour lines drawn into a rendered tile from the region's height files.
 *
 * Height files: one per z12 tile, "<region>/12/<x>/<y>.hgt" next to the tile's .pbf, made from the AWS Terrain
 * Tiles PNG when it is downloaded: a 16-byte header ("TMHG", version, zoom, size) and 256 x 256 heights as 16-bit
 * little-endian codes (Terrain.h), rows north to south. A value is the height at the pixel's top-left corner (half a
 * pixel off the PNG's centre, 13 m at z12: below what the data resolves).
 *
 * A tile of zoom 12 or more reads only the part of its z12 file it covers, plus the first column and row of the
 * neighbours to the east and south so lines meet across tile edges: at most 129 x 129 points (z12 and z13 read every
 * second or every point, z14 64 x 64 cells, deeper zooms fewer).
 */
constexpr int TERRAIN_ZOOM = 12;
constexpr int TERRAIN_PATCH_CELLS = 128;
constexpr int TERRAIN_PATCH_POINTS = TERRAIN_PATCH_CELLS + 1;
/**
 * A patch is stored with one more point on each side (from the neighbour files where it reaches over the file's
 * edge), so the slopes at a tile's edge are the same as in the tile next to it: no seam in the hillshade.
 * Point (i, j) of the patch, i and j from -1 to cells + 1, is grid[(j + 1) * TERRAIN_GRID_STRIDE + i + 1].
 */
constexpr int TERRAIN_GRID_STRIDE = TERRAIN_PATCH_POINTS + 2;
constexpr size_t TERRAIN_FILE_HEADER = 16;
constexpr size_t TERRAIN_FILE_BYTES = TERRAIN_FILE_HEADER + static_cast<size_t>(TERRAIN_TILE_SIZE) * TERRAIN_TILE_SIZE * 2;
constexpr uint8_t TERRAIN_FILE_VERSION = 1;
/** Height file rows are read in blocks of this size. */
constexpr size_t TERRAIN_READ_BYTES = 16 * 1024;
/** The buffer readTerrainPatch() needs: three row blocks (west, own, east file) and three single rows. */
constexpr size_t TERRAIN_ROW_BUFFER_BYTES = 3 * TERRAIN_READ_BYTES + 3 * static_cast<size_t>(TERRAIN_TILE_SIZE) * 2;

void writeTerrainHeader(uint8_t* header);
/**
 * Turns a downloaded Terrarium PNG into a height file: file has TERRAIN_FILE_BYTES (4-byte aligned), rowBuffer 2 KB.
 * Both targets are little-endian, so the decoded grid is the file's body as it is.
 */
bool terrariumToHeightFile(ByteInput& png, InflateReader& inflater, uint8_t* rowBuffer, uint8_t* file);
bool checkTerrainHeader(const uint8_t* header);

/** Which points of a z12 height file a tile needs. */
struct TerrainSpan {
    /** The z12 tile whose file holds the patch. */
    TileKey dem;
    /** First point in that file, the step between points read, and the number of cells across (points - 1). */
    int x0;
    int y0;
    int step;
    int cells;
};

/** False below zoom 12. */
bool terrainSpan(const TileKey& target, TerrainSpan& span);

/**
 * Which of the 3 x 3 height files around the patch's own one (index (dy + 1) * 3 + dx + 1, own = 4) its points and
 * margin reach into: the others need not be opened.
 */
void terrainFilesNeeded(const TerrainSpan& span, bool needed[9]);

/** Opens height file index (as in terrainFilesNeeded()) for reading, nullptr when it is not on the card. */
using TerrainFileOpener = FILE* (*)(int index, void* context);

/**
 * Reads a patch with its margin (TERRAIN_GRID_STRIDE): file 4 is the tile's own height file, the others its
 * neighbours as in terrainFilesNeeded() (one not on the card: the nearest point read stands in). At most three files
 * are open at a time (the card allows four), and each is closed here. rowBuffer has TERRAIN_ROW_BUFFER_BYTES. False
 * when the own file is missing or damaged.
 */
bool readTerrainPatch(TerrainFileOpener open, void* context, const TerrainSpan& span, uint16_t* grid, uint8_t* rowBuffer);

/**
 * The height at a point of a z12 height file, x and y in its pixels (0-256), between the four points around it.
 * False when the file is not a height file.
 */
bool readTerrainHeight(FILE* file, double x, double y, float& metres);
/** The same from a whole height file in memory (TERRAIN_FILE_BYTES, header checked by the caller). */
float terrainHeightIn(const uint8_t* file, double x, double y);

/** Ground metres of one patch cell at the latitude of the target tile. */
float terrainCellMetres(const TileKey& target, const TerrainSpan& span);

/** Contour intervals (minor, major) in metres for the zoom level of the drawn tile. */
void contourIntervals(int zoom, float& minorM, float& majorM);

/**
 * The working memory of one render lane and the drawing. The patch is filled by the lane (readTerrainPatch), then
 * the renderer calls draw() after the parks and before water.
 */
class TerrainOverlay {
public:
    TerrainOverlay() = default;
    ~TerrainOverlay();
    TerrainOverlay(const TerrainOverlay&) = delete;
    TerrainOverlay& operator=(const TerrainOverlay&) = delete;

    bool begin();
    void end();
    bool isReady() const { return grid != nullptr; }

    uint16_t* patch() { return grid; }
    uint8_t* rowBuffer() { return row; }

    /** The patch read for the tile about to be drawn; cleared after each drawing. */
    void setPatch(int cells, float cellMetres) {
        patchCells = cells;
        metresPerCell = cellMetres;
    }
    bool hasPatch() const { return patchCells > 0; }
    /** The light step of the turned map (terrainLightStep), for the tiles drawn from now on. */
    void setLightStep(int step) { lightStep = step; }
    void clearPatch() { patchCells = 0; }

    /** Hillshade (colour palettes) and contours (not on 1-bit displays) into a 256 x 256 tile. */
    void draw(uint16_t* pixels, int zoom, const Palette& palette, Raster& raster);

    /** Lines drawn in the last draw(), for logs and tests. */
    int lastLines() const { return lines; }
    /** Height numbers put on major lines in the last draw() (zoom 13 and deeper). */
    int lastLabels() const { return labelCount; }

private:
    void shadePixels(uint16_t* pixels, const Palette& palette);
    void drawContours(int zoom, const Palette& palette, Raster& raster, uint16_t* pixels);
    static void onLine(void* context, float levelM, const float* points, int pointCount, bool closed);

    uint16_t* grid = nullptr;
    uint8_t* shade = nullptr;
    uint8_t* row = nullptr;
    float* simplified = nullptr;
    ContourTracer* tracer = nullptr;
    int patchCells = 0;
    int lightStep = 0;
    float metresPerCell = 0.0f;
    int lines = 0;
    /** State of the running contour pass. */
    Raster* passRaster = nullptr;
    float passScale = 1.0f;
    float passHalfWidth = 0.5f;
    float passMajorM = 100.0f;
    bool passMajor = false;
    /** Height numbers for the major lines: the middle of each long one, drawn after the lines. */
    struct ContourLabel {
        float x;
        float y;
        int metres;
    };
    static constexpr int MAX_CONTOUR_LABELS = 6;
    ContourLabel contourLabels[MAX_CONTOUR_LABELS] {};
    int labelCount = 0;
    bool passLabels = false;
    void drawLabels(uint16_t* pixels, uint16_t ink, uint16_t halo);
};

} // namespace maps
