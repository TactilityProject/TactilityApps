// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"

#include "../platform/Platform.h"
#include "../render/PoiIconData.h"
#include "../render/PoiTypes.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace maps {

namespace {

/** The icon mask of each POI type, found once by name in the generated Maki masks. */
const uint8_t* poiIcons[256] {};
bool poiMonochrome = false;
uint16_t poiMonochromeColor = 0;

const uint8_t* iconMask(const char* name) {
    int low = 0;
    int high = POI_ICON_COUNT - 1;
    while (low <= high) {
        const int middle = (low + high) / 2;
        const int order = strcmp(POI_ICON_MASKS[middle].name, name);
        if (order == 0) {
            return POI_ICON_MASKS[middle].alpha;
        }
        if (order < 0) {
            low = middle + 1;
        } else {
            high = middle - 1;
        }
    }
    return nullptr;
}

/** Icon colours by group, dark enough to read on the light map with a white halo. */
uint16_t groupColor(PoiGroup group) {
    switch (group) {
        case PoiGroup::Food: return 0xC300;      // orange
        case PoiGroup::Shop: return 0x9113;      // purple
        case PoiGroup::Transport: return 0x2A5A; // blue
        case PoiGroup::Health: return 0xC104;    // red
        case PoiGroup::Money: return 0x3A8A;     // slate
        case PoiGroup::Lodging: return 0x0451;   // teal
        case PoiGroup::Culture: return 0x8209;   // plum brown
        case PoiGroup::Leisure: return 0x3445;   // green
        case PoiGroup::Education: return 0x8A42; // brown
        case PoiGroup::Worship: return 0x4208;   // dark grey
        default: return 0x52AA;                  // grey
    }
}

void preparePoiIcons(bool monochrome, uint16_t monochromeColor) {
    poiMonochrome = monochrome;
    poiMonochromeColor = monochromeColor;
    const uint8_t* fallback = iconMask("marker");
    for (int id = 1; id <= poiTypeCount() && id < 256; id++) {
        const uint8_t* mask = iconMask(poiIconName(poiType(static_cast<uint8_t>(id)).icon));
        poiIcons[id] = mask != nullptr ? mask : fallback;
    }
}

bool lookupPoi(uint8_t id, PoiAppearance& out) {
    if (id == 0 || id > poiTypeCount() || poiIcons[id] == nullptr) {
        return false;
    }
    const PoiType& type = poiType(id);
    out = { poiIcons[id], POI_ICON_SIZE, poiMonochrome ? poiMonochromeColor : groupColor(type.group), type.minZoom };
    return true;
}

} // namespace

void MapView::applyLabelFonts() {
    LabelFont* small = labelFonts[0].isOpen() ? &labelFonts[0] : (smallFont.isOpen() ? &smallFont : &normalFont);
    LabelFont* normal = labelFonts[1].isOpen() ? &labelFonts[1] : static_cast<LabelFont*>(&normalFont);
    LabelFont* large = labelFonts[2].isOpen() ? &labelFonts[2] : (largeFont.isOpen() ? &largeFont : normal);
    labels.setFonts(small, normal, large);
    // The letters most names use are unpacked into the glyph caches now, not while the first view's labels are placed
    // (about 4 ms of a z18 placement, 16 Nov). The large font is left: few labels use it and its masks fill the cache.
    const uint32_t start = nowMs();
    for (LabelFont* font : { small, normal }) {
        LabelGlyph glyph {};
        for (uint32_t c = 32; c < 127; c++) {
            font->glyph(c, glyph, true);
        }
    }
    logInfo("Label glyphs ready in %u ms", static_cast<unsigned>(nowMs() - start));
}

void MapView::pollLabelFonts() {
    // Only the map loop touches the label layout: the maker's task just reports, and the files are opened here
    if (labelFontsApplied || !fontMaker.isFinished()) {
        return;
    }
    labelFontsApplied = true;
    fontMaker.stop();
    const unsigned written = fontMaker.writtenMask();
    auto openWritten = [&] {
        for (int i = 0; i < LabelFontMaker::COUNT; i++) {
            char fontPath[200];
            LabelFontMaker::cachePath(rootPath, i, fontPath, sizeof(fontPath));
            if ((written & (1u << i)) != 0 && !labelFonts[i].openFile(fontPath)) {
                logError("Label font %s does not open, the map keeps the system font", fontPath);
            }
        }
        return true;
    };
    runOnCard(io, openWritten);
    applyLabelFonts();
    labelZoom = -1;
}

void MapView::applyLabelSettings() {
    labels.setDensity(static_cast<LabelDensity>(std::clamp(settings.labels, 0, 2)));
    preparePoiIcons(monochrome, palette->text);
    labels.setPoiLookup(lookupPoi);
    labels.setPreferLatin(settings.latinNames);
    const bool binary = palette->binary;
    // Dark names with a light halo, water in blue, peaks in brown; black on white on monochrome displays
    labels.setStyle({ monochrome ? palette->text : static_cast<uint16_t>(0x2124), monochrome ? palette->text : static_cast<uint16_t>(0x4208),
        monochrome ? palette->text : static_cast<uint16_t>(0x2A35), monochrome ? palette->text : static_cast<uint16_t>(0x6A04),
        monochrome ? palette->halo : static_cast<uint16_t>(0xFFFF), binary, monochrome ? palette->text : static_cast<uint16_t>(0x7BEF),
        palette->outdoor && !monochrome });
    labelZoom = -1;
}

void MapView::placeLabels() {
    const uint32_t start = nowMs();
    double left;
    double top;
    double right;
    double bottom;
    viewBounds(left, top, right, bottom);
    // With the map position of the screen's corner the placement does not depend on where the view was panned to
    labels.beginTurnedView(width, height, zoom, left, top, centerX, centerY, bearingCos, bearingSin);
    // House numbers show from z17 (z18 with few labels): only then are they read from the label files
    labelStore.setNumbersWanted(zoom >= 17 + (settings.labels == static_cast<int>(LabelDensity::Few) ? 1 : 0));
    const int tileCount = 1 << zoom;
    TileKey sources[32];
    int sourceCount = 0;
    for (int y = static_cast<int>(std::floor(top / TILE_SIZE)); y <= static_cast<int>(std::floor((bottom - 1) / TILE_SIZE)); y++) {
        if (y < 0 || y >= tileCount) {
            continue;
        }
        for (int x = static_cast<int>(std::floor(left / TILE_SIZE)); x <= static_cast<int>(std::floor((right - 1) / TILE_SIZE)); x++) {
            const TileKey tile { zoom, wrapTileX(x, zoom), y };
            // The label section of the tile it was drawn from: its own up to z14, its z14 ancestor above, or a coarser
            // one where the card has no detail
            for (int sourceZoom = std::min(zoom, MAX_SOURCE_ZOOM); sourceZoom >= std::max(0, std::min(zoom, MAX_SOURCE_ZOOM) - 2); sourceZoom--) {
                const TileKey source = ancestorOf(tile, sourceZoom);
                bool known = false;
                for (int i = 0; i < sourceCount && !known; i++) {
                    known = sources[i] == source;
                }
                if (known) {
                    break;
                }
                const double size = std::ldexp(static_cast<double>(TILE_SIZE), zoom - sourceZoom);
                // The source's corner from the screen centre, from the visible tile's own column so the antimeridian
                // does not matter, then turned onto the screen
                const double cornerX = x * TILE_SIZE - (tile.x - (source.x << (zoom - sourceZoom))) * TILE_SIZE - centerX;
                const double cornerY = source.y * size - centerY;
                double screenX;
                double screenY;
                worldToScreenOffset(cornerX, cornerY, screenX, screenY);
                const auto scale = static_cast<float>(size / PREPARED_EXTENT);
                const LabelTransform transform { scale * bearingCos, scale * bearingSin, -scale * bearingSin, scale * bearingCos,
                    static_cast<float>(width / 2.0 + screenX), static_cast<float>(height / 2.0 + screenY) };
                const bool found = labelStore.with(source, [&](const uint8_t* bytes, size_t size, int count, const LabelCells* cells) {
                    labels.addSection(bytes, size, count, transform, cells);
                });
                if (found) {
                    if (sourceCount < 32) {
                        sources[sourceCount++] = source;
                    }
                    break;
                }
            }
        }
    }
    const uint32_t added = nowMs();
    const int placedCount = labels.place();
    labelCenterX = centerX;
    labelCenterY = centerY;
    labelZoom = zoom;
    labelBearing = bearing;
    lastLabelPlace = nowMs();
    logInfo("Labels: %d placed of %d candidates (%d records) from %d tiles in %u ms (reading %u, placing %u, of that drawing %u)", placedCount,
        labels.candidateTotal(), labels.recordsRead(), sourceCount, static_cast<unsigned>(nowMs() - start), static_cast<unsigned>(added - start),
        static_cast<unsigned>(nowMs() - added), static_cast<unsigned>(labels.spriteMicros() / 1000));
}

void MapView::updateLabels(uint32_t now) {
    if (!labelsReady || settings.labels == 0 || animation.active || turn.active || pinching || isFullScreen()) {
        return;
    }
    // Placed again once the view settled after a change, or when tiles brought new label data; upright again after a turn
    const bool moved = labelZoom != zoom || labelBearing != bearing || std::fabs(labelCenterX - centerX) > 0.5 ||
        std::fabs(labelCenterY - centerY) > 0.5;
    const bool newData = labelStore.generation() != labelGeneration;
    // New data alone (often tiles drawn for the ring around the screen) places them again at most every 1.5 s
    if ((!moved && !newData) || now - lastInputAt < 200 || now - lastLabelPlace < (moved ? 300u : 1500u)) {
        return;
    }
    labelGeneration = labelStore.generation();
    placeLabels();
    dirty = true;
}

} // namespace maps
