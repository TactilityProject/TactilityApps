// SPDX-License-Identifier: GPL-3.0-or-later
// Converts the vector tiles of a folder into prepared tiles and compares them with the originals:
// size, render time and how different the rendered pixels are.
// Usage: prepare_tiles <dir> [--write] [--deflate] [--zoom <z>]
// <dir> is a folder with maps/<region>/<z>/<x>/<y>.pbf inside. --write stores <y>.prt next to each original,
// deflated with --deflate. --zoom renders at another zoom level (overzoom) instead of the tile's own.

#include "../main/Source/render/PreparedTile.h"
#include "../main/Source/render/TileRenderer.h"
#include "ImageDiff.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace maps;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> readFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

struct Totals {
    int tiles = 0;
    uint64_t pbfBytes = 0;
    uint64_t rawBytes = 0;
    uint64_t deflatedBytes = 0;
    double pbfMs = 0;
    double pbfGeometryMs = 0;
    double pbfFillMs = 0;
    double pbfBuildingMs = 0;
    double rawFillMs = 0;
    double rawBuildingMs = 0;
    double rawMs = 0;
    double rawGeometryMs = 0;
    double deflatedMs = 0;
    double convertMs = 0;
    double worstMean = 0;
    double worstStrong = 0;
};

/** Renders a few times and keeps the fastest, so the numbers are steady. */
template <typename Render>
RenderStats fastest(Render render) {
    RenderStats best {};
    best.totalMs = UINT32_MAX;
    for (int i = 0; i < 3; i++) {
        RenderStats stats {};
        if (!render(stats)) {
            return {};
        }
        if (stats.totalMs < best.totalMs) {
            best = stats;
        }
    }
    return best;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: prepare_tiles <dir> [--write] [--deflate] [--zoom <z>]\n");
        return 2;
    }
    bool write = false;
    bool deflateWritten = false;
    int zoom = -1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--write") == 0) {
            write = true;
        } else if (strcmp(argv[i], "--deflate") == 0) {
            deflateWritten = true;
        } else if (strcmp(argv[i], "--zoom") == 0 && i + 1 < argc) {
            zoom = atoi(argv[++i]);
        }
    }
    auto* renderer = new TileRenderer();
    auto* converter = new TileConverter();
    if (!renderer->init()) {
        return 1;
    }
    std::vector<uint16_t> original(TILE_SIZE * TILE_SIZE);
    std::vector<uint16_t> prepared(TILE_SIZE * TILE_SIZE);
    Totals totals;
    printf("%-14s %8s %8s %8s %6s %6s | %7s %7s %7s | %6s %6s\n", "tile", "pbf", "raw", "deflate", "raw%", "defl%",
        "pbf ms", "raw ms", "defl ms", "mean", "strong");
    for (const auto& entry : fs::recursive_directory_iterator(argv[1])) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pbf") {
            continue;
        }
        const fs::path& path = entry.path();
        const TileKey source { atoi(path.parent_path().parent_path().filename().string().c_str()),
            atoi(path.parent_path().filename().string().c_str()), atoi(path.stem().string().c_str()) };
        TileKey target = source;
        if (zoom > source.z) {
            // The top left descendant at the requested zoom level
            target = { zoom, source.x << (zoom - source.z), source.y << (zoom - source.z) };
        }
        const std::vector<uint8_t> pbf = readFile(path);
        ByteWriter raw;
        ByteWriter deflated;
        ConvertStats convertStats {};
        MemoryInput input(pbf.data(), pbf.size());
        if (!converter->convert(renderer->tileDecoder(), &input, source, static_cast<uint32_t>(pbf.size()), "host", false, raw, &convertStats) ||
            !converter->convert(renderer->tileDecoder(), &input, source, static_cast<uint32_t>(pbf.size()), "host", true, deflated, nullptr)) {
            fprintf(stderr, "%s: conversion failed\n", path.string().c_str());
            continue;
        }
        const RenderStats pbfStats = fastest([&](RenderStats& stats) {
            MemoryInput again(pbf.data(), pbf.size());
            return renderer->render(&again, source, target, colorPalette(), original.data(), &stats);
        });
        const RenderStats deflatedStats = fastest([&](RenderStats& stats) {
            return renderer->renderPrepared(deflated.bytes(), deflated.size(), target, colorPalette(), prepared.data(), &stats);
        });
        const RenderStats rawStats = fastest([&](RenderStats& stats) {
            return renderer->renderPrepared(raw.bytes(), raw.size(), target, colorPalette(), prepared.data(), &stats);
        });
        const ImageDiff diff = compareImages(original.data(), prepared.data(), original.size());
        char name[32];
        snprintf(name, sizeof(name), "%d/%d/%d", source.z, source.x, source.y);
        printf("%-14s %8zu %8zu %8zu %5.1f%% %5.1f%% | %7u %7u %7u | %6.2f %5.2f%%\n", name, pbf.size(), raw.size(), deflated.size(),
            100.0 * raw.size() / pbf.size(), 100.0 * deflated.size() / pbf.size(), pbfStats.totalMs, rawStats.totalMs, deflatedStats.totalMs,
            diff.meanDifference, 100.0 * diff.strongShare);
        printf("               features %u -> %u, points %u -> %u, converted in %u ms; geometry %u -> %u ms, fill %u -> %u ms\n",
            convertStats.featuresIn, convertStats.featuresKept, convertStats.pointsIn, convertStats.pointsKept, convertStats.totalMs,
            pbfStats.geometryMs, rawStats.geometryMs, pbfStats.fillMs, rawStats.fillMs);
        totals.tiles++;
        totals.pbfBytes += pbf.size();
        totals.rawBytes += raw.size();
        totals.deflatedBytes += deflated.size();
        totals.pbfMs += pbfStats.totalMs;
        totals.pbfGeometryMs += pbfStats.geometryUs / 1000.0;
        totals.pbfFillMs += pbfStats.fillUs / 1000.0;
        totals.pbfBuildingMs += pbfStats.buildingUs / 1000.0;
        totals.rawFillMs += rawStats.fillUs / 1000.0;
        totals.rawBuildingMs += rawStats.buildingUs / 1000.0;
        totals.rawMs += rawStats.totalMs;
        totals.rawGeometryMs += rawStats.geometryUs / 1000.0;
        totals.deflatedMs += deflatedStats.totalMs;
        totals.convertMs += convertStats.totalMs;
        totals.worstMean = diff.meanDifference > totals.worstMean ? diff.meanDifference : totals.worstMean;
        totals.worstStrong = diff.strongShare > totals.worstStrong ? diff.strongShare : totals.worstStrong;
        if (write) {
            const ByteWriter& chosen = deflateWritten ? deflated : raw;
            fs::path out = path;
            out.replace_extension(".prt");
            std::ofstream file(out, std::ios::binary);
            file.write(reinterpret_cast<const char*>(chosen.bytes()), static_cast<std::streamsize>(chosen.size()));
        }
    }
    if (totals.tiles == 0) {
        fprintf(stderr, "No tiles found\n");
        return 1;
    }
    const double n = totals.tiles;
    printf("\n%d tiles: .prt/.pbf size raw %.1f%%, deflated %.1f%%\n", totals.tiles, 100.0 * totals.rawBytes / totals.pbfBytes,
        100.0 * totals.deflatedBytes / totals.pbfBytes);
    printf("mean per tile: pbf %.0f KB, raw %.0f KB, deflated %.0f KB\n", totals.pbfBytes / n / 1024, totals.rawBytes / n / 1024,
        totals.deflatedBytes / n / 1024);
    printf("mean render: pbf %.1f ms (geometry %.2f, fill %.2f, buildings %.2f), raw %.1f ms (geometry %.2f, fill %.2f, buildings %.2f), "
        "deflated %.1f ms; conversion %.1f ms\n",
        totals.pbfMs / n, totals.pbfGeometryMs / n, totals.pbfFillMs / n, totals.pbfBuildingMs / n, totals.rawMs / n, totals.rawGeometryMs / n,
        totals.rawFillMs / n, totals.rawBuildingMs / n, totals.deflatedMs / n, totals.convertMs / n);
    printf("worst pixel difference: mean %.2f of 255, strong %.2f%%\n", totals.worstMean, 100.0 * totals.worstStrong);
    delete converter;
    delete renderer;
    return 0;
}
