// SPDX-License-Identifier: GPL-3.0-or-later
// Draws the app's screens on the PC with the same UI code, over a real map, so layouts can be checked without
// a device. Text is drawn as blocks (FakeFont). Usage: ui_preview <out folder> [--mono]

#include "../main/Source/render/Compose.h"
#include "../main/Source/render/TileRenderer.h"
#include "../main/Source/ui/Canvas.h"
#include "../main/Source/ui/Controls.h"
#include "../main/Source/ui/DownloadFrame.h"
#include "../main/Source/ui/MapOverlay.h"
#include "../main/Source/ui/Widgets.h"
#include "FakeFont.h"
#include "Png.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace maps;

#ifndef FIXTURE_DIR
#define FIXTURE_DIR "../tests/fixtures"
#endif

namespace {

constexpr int WIDTH = 320;
constexpr int HEIGHT = 240;

struct Preview {
    std::vector<uint16_t> map;
    std::vector<uint16_t> pixels;
    Canvas canvas;
    FakeFont font;
    MapOverlay overlay;
    const Palette* palette;
    UiTheme theme;
    bool mono;
    std::string folder;

    Preview(const std::string& out, bool monochrome) : map(WIDTH * HEIGHT), pixels(WIDTH * HEIGHT), mono(monochrome), folder(out) {
        canvas = { pixels.data(), WIDTH, HEIGHT };
        palette = mono ? &monochromePalette() : &colorPalette();
        theme = uiTheme(*palette, mono);
        overlay.setFonts(&font, &font);
    }

    void startFromMap() { pixels = map; }

    void drawMapOverlay(bool fullAttribution) {
        overlay.setBottomInset(0);
        overlay.drawAccuracyCircle(canvas, 150, 110, 40.0f, *palette);
        overlay.drawMarker(canvas, 150, 110, true, 45.0f, *palette);
        overlay.drawScaleBar(canvas, 9.5, *palette);
        if (fullAttribution) {
            int x;
            int y;
            int w;
            int h;
            overlay.drawAttribution(canvas, *palette, x, y, w, h);
        } else {
            overlay.drawAttributionMark(canvas, *palette);
        }
    }

    void save(const char* name) {
        const std::string path = folder + "/" + name + (mono ? "-mono" : "") + ".png";
        if (writePngRgb565(path.c_str(), pixels.data(), WIDTH, HEIGHT)) {
            printf("wrote %s\n", path.c_str());
        }
    }
};

std::vector<uint8_t> readFile(const char* path) {
    std::vector<uint8_t> bytes;
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        return bytes;
    }
    uint8_t buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        bytes.insert(bytes.end(), buffer, buffer + count);
    }
    fclose(file);
    return bytes;
}

struct OneTile {
    std::vector<uint16_t> pixels;
};

const uint16_t* lookup(const TileKey& tile, void* context) {
    auto* one = static_cast<OneTile*>(context);
    return tile == TileKey({ 14, 9058, 5729 }) ? one->pixels.data() : nullptr;
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: ui_preview <out folder> [--mono]\n");
        return 2;
    }
    const bool mono = argc > 2 && strcmp(argv[2], "--mono") == 0;
    auto* preview = new Preview(argv[1], mono);

    // The fixture tile, drawn into the middle of the screen
    const std::vector<uint8_t> pbf = readFile(FIXTURE_DIR "/14-9058-5729.pbf");
    auto* renderer = new TileRenderer();
    OneTile tile;
    tile.pixels.resize(TILE_SIZE * TILE_SIZE);
    MemoryInput input(pbf.data(), pbf.size());
    if (!renderer->init() || !renderer->render(&input, { 14, 9058, 5729 }, { 14, 9058, 5729 }, *preview->palette, tile.pixels.data(), nullptr)) {
        fprintf(stderr, "Could not render the fixture tile\n");
        return 1;
    }
    composeView(preview->map.data(), WIDTH, HEIGHT, 9058.5 * TILE_SIZE, 5729.5 * TILE_SIZE, 14, lookup, &tile, preview->palette->placeholder);
    Canvas& canvas = preview->canvas;
    Font& font = preview->font;
    const UiTheme& theme = preview->theme;

    // The map as it opens, with the full attribution, and a toast
    preview->startFromMap();
    preview->drawMapOverlay(true);
    preview->save("map-start");
    preview->startFromMap();
    preview->drawMapOverlay(false);
    preview->overlay.drawToast(canvas, "Location is off. Enable it in Tactility's Settings.", theme);
    preview->save("map-toast");
    // An approximate (IP) position: a ring and a label, no accuracy circle
    preview->startFromMap();
    preview->overlay.drawApproximateMarker(canvas, 150, 110, 25000.0f, *preview->palette);
    preview->overlay.drawAttributionMark(canvas, *preview->palette);
    preview->save("map-approximate");

    // The control overlay: with key hints (T-Deck), touch only, and focus on a board with buttons only
    Controls controls;
    controls.layout(WIDTH, HEIGHT, preview->overlay.scaleBarTop(canvas));
    controls.setInputs({ true, true, false });
    controls.show();
    preview->startFromMap();
    preview->drawMapOverlay(false);
    controls.draw(canvas, font, theme, PositionButton::Free);
    preview->save("controls-keys");
    controls.setInputs({ false, true, false });
    preview->startFromMap();
    preview->drawMapOverlay(false);
    controls.draw(canvas, font, theme, PositionButton::Following);
    preview->save("controls-touch");
    controls.setInputs({ true, false, false });
    controls.show();
    controls.moveFocus(3);
    preview->startFromMap();
    preview->drawMapOverlay(false);
    controls.draw(canvas, font, theme, PositionButton::Unavailable);
    preview->save("controls-focus");

    // Download area: fixed frame, map under it, info bar with the Download button
    const DownloadLayout layout = downloadLayout(WIDTH, HEIGHT, font.lineHeight());
    const double centerX = 9058.5 * TILE_SIZE;
    const double centerY = 5729.5 * TILE_SIZE;
    preview->startFromMap();
    drawDownloadScreen(canvas, font, theme, layout, planDownload(screenBounds(layout.frame, centerX, centerY, 14, WIDTH, HEIGHT), MAX_SOURCE_ZOOM));
    preview->save("download");

    // Menu popup over the map
    ListView menu;
    for (const char* label : { "Download area", "Regions", "Settings", "About and licences" }) {
        menu.add(label);
    }
    menu.layoutPopup(WIDTH, HEIGHT, font, 220);
    menu.select(1);
    preview->startFromMap();
    preview->drawMapOverlay(false);
    menu.draw(canvas, font, theme);
    preview->save("menu");

    // Regions, with more rows than fit, and the delete question
    ListView regions;
    regions.setTitle("< Regions");
    regions.setHint("Enter: show  Del: delete  Esc: back");
    const char* names[] = { "area01  complete", "area02  downloading 42%", "budapest  outdated", "szeged  complete", "area05  paused 10%",
        "vienna  complete", "area07  complete", "area08  complete" };
    for (const char* name : names) {
        regions.add(name, "12.3 MB 26-10-29");
    }
    regions.layoutFullScreen(WIDTH, HEIGHT, font);
    regions.select(6);
    regions.draw(canvas, font, theme);
    preview->save("regions");
    Dialog dialog;
    dialog.open("Delete area07? Tiles other regions use are kept.", "Delete", true);
    dialog.layout(WIDTH, HEIGHT, font);
    dialog.draw(canvas, font, theme);
    preview->save("regions-delete");

    // Settings
    ListView settings;
    settings.setTitle("< Settings");
    settings.setHint("Enter or tap: change  Esc: back");
    settings.add("SD card cache", "On");
    settings.add("Prepared tiles", "Deflated");
    settings.add("Smooth pinch", "On");
    settings.layoutFullScreen(WIDTH, HEIGHT, font);
    settings.draw(canvas, font, theme);
    preview->save("settings");

    delete renderer;
    delete preview;
    return 0;
}
