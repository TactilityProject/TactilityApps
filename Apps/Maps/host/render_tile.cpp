// SPDX-License-Identifier: GPL-3.0-or-later
// Renders a view of the map around a coordinate to a PNG file, with the same code the app uses.
// Usage: render_tile <dir> <lat> <lon> <zoom> <width> <height> <out.png> [--mono] [--prepared]
// <dir> is a folder with maps/<region>/<z>/<x>/<y>.pbf inside, or one region folder. --prepared draws from the
// .prt copies next to the originals where they are current (see prepare_tiles --write).

#include "../main/Source/render/Compose.h"
#include "../main/Source/render/PreparedTile.h"
#include "../main/Source/render/TileRenderer.h"
#include "../main/Source/tiles/TileStore.h"
#include "Png.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <vector>

using namespace maps;

namespace {

struct Context {
    TileStore store;
    TileRenderer renderer;
    const Palette* palette;
    bool prepared = false;
    std::map<uint64_t, std::vector<uint16_t>> tiles;
    uint32_t totalMs = 0;
};

uint64_t keyOf(const TileKey& tile) {
    return (static_cast<uint64_t>(tile.z) << 58) | (static_cast<uint64_t>(tile.x) << 29) | static_cast<uint64_t>(tile.y);
}

const uint16_t* lookup(const TileKey& tile, void* data) {
    auto* context = static_cast<Context*>(data);
    const auto found = context->tiles.find(keyOf(tile));
    if (found != context->tiles.end()) {
        return found->second.empty() ? nullptr : found->second.data();
    }
    std::vector<uint16_t>& pixels = context->tiles[keyOf(tile)];
    TileFile file {};
    if (!context->store.find(tile, file)) {
        printf("tile %d/%d/%d: no data\n", tile.z, tile.x, tile.y);
        return nullptr;
    }
    const TileKey source = file.tile;
    pixels.resize(TILE_SIZE * TILE_SIZE);
    RenderStats stats {};
    char preparedFile[PATH_MAX_LENGTH];
    FileBuffer buffer;
    PreparedHeader header {};
    if (context->prepared && preparedPath(preparedFile, sizeof(preparedFile), context->store.root(), context->store.regionName(file.region), source) &&
        buffer.load(preparedFile) && readPreparedHeader(buffer.bytes(), buffer.size(), header) && isPreparedCurrent(header, source, file.size)) {
        if (!context->renderer.renderPrepared(buffer.bytes(), buffer.size(), tile, *context->palette, pixels.data(), &stats)) {
            pixels.clear();
            return nullptr;
        }
        printf("tile %d/%d/%d from .prt: ", tile.z, tile.x, tile.y);
    } else {
        FileInput input;
        if (!input.open(file.path)) {
            return nullptr;
        }
        if (!context->renderer.render(&input, source, tile, *context->palette, pixels.data(), &stats)) {
            pixels.clear();
            return nullptr;
        }
    }
    context->totalMs += stats.totalMs;
    printf("tile %d/%d/%d from %d/%d/%d: %u ms (scan %u ms), %u sweeps, %u features, %u bytes inflated\n",
        tile.z, tile.x, tile.y, source.z, source.x, source.y, stats.totalMs, stats.scanMs, stats.sweeps, stats.features, stats.decompressedBytes);
    return pixels.data();
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 8) {
        fprintf(stderr, "Usage: render_tile <dir> <lat> <lon> <zoom> <width> <height> <out.png> [--mono]\n");
        return 2;
    }
    const double lat = atof(argv[2]);
    const double lon = atof(argv[3]);
    const int zoom = atoi(argv[4]);
    const int width = atoi(argv[5]);
    const int height = atoi(argv[6]);
    bool mono = false;
    bool prepared = false;
    for (int i = 8; i < argc; i++) {
        mono = mono || strcmp(argv[i], "--mono") == 0;
        prepared = prepared || strcmp(argv[i], "--prepared") == 0;
    }
    if (zoom < 0 || zoom > MAX_VIEW_ZOOM || width <= 0 || height <= 0 || width > 8192 || height > 8192) {
        fprintf(stderr, "Invalid zoom or size\n");
        return 2;
    }

    auto* context = new Context();
    context->palette = mono ? &monochromePalette() : &colorPalette();
    context->prepared = prepared;
    const std::filesystem::path dir(argv[1]);
    if (std::filesystem::is_directory(dir / "maps")) {
        context->store.setRoot(dir.string().c_str());
        context->store.scanRegions();
    } else {
        context->store.setRoot(dir.parent_path().parent_path().string().c_str());
        context->store.addRegion(dir.filename().string().c_str());
    }
    if (context->store.regionCount() == 0) {
        fprintf(stderr, "No regions found in %s\n", argv[1]);
        return 1;
    }
    if (!context->renderer.init()) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    std::vector<uint16_t> view(static_cast<size_t>(width) * height);
    const int missing = composeView(view.data(), width, height, lonToWorldX(lon, zoom), latToWorldY(lat, zoom), zoom, lookup, context, context->palette->placeholder);
    printf("rendered %zu tiles in %u ms, %d missing\n", context->tiles.size(), context->totalMs, missing);
    if (!writePngRgb565(argv[7], view.data(), width, height)) {
        fprintf(stderr, "Could not write %s\n", argv[7]);
        return 1;
    }
    delete context;
    return 0;
}
