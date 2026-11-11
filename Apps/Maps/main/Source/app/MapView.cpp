// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"

#include <app/paths.h>

#include "../platform/Platform.h"
#include "../render/Compose.h"
#include "../render/PoiIconData.h"
#include "../render/PoiTypes.h"

#include <lvgl.h>
#include <lvgl/fonts.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/power_supply.h>
#include <tactility/paths.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

/**
 * 2.5 MB of rendered tiles in PSRAM, enough for a screen and its neighbours at two zoom levels. With 28 a download of
 * low-zoom tiles (up to 1.2 MB each) started with only 0.5 MB of PSRAM free and failed.
 */
constexpr int TILE_CACHE_SLOTS = 20;
constexpr uint32_t LOOP_INTERVAL_MS = 15;
/** How long a +/- or double tap zoom step is animated. */
constexpr float ZOOM_ANIMATION_MS = 220.0f;
constexpr uint32_t POINTER_HIDE_MS = 3000;
/** Idle-time updates start after this long without input, on Wi-Fi, charging or above UPDATE_MIN_BATTERY percent. */
constexpr uint32_t UPDATE_IDLE_MS = 30000;
constexpr uint32_t POWER_CHECK_MS = 10000;
constexpr int UPDATE_MIN_BATTERY = 40;
/** The map opens at ESA ESTEC, Noordwijk, the first time, then where it was left. */
constexpr double DEFAULT_LAT = 52.2186;
constexpr double DEFAULT_LON = 4.4206;
constexpr int DEFAULT_ZOOM = 14;
/** Prefetch and other idle work start after this long without input. */
constexpr uint32_t IDLE_AFTER_MS = 1000;
/** Location is read on each signal of the location service, and every few seconds to notice it being switched off. */
constexpr uint32_t LOCATION_CHECK_MS = 3000;
constexpr uint32_t TOAST_SHOW_MS = 3500;
constexpr uint32_t STATUS_POLL_MS = 500;
constexpr uint32_t MESSAGE_SHOW_MS = 4000;
/** A failed download says so longer, and tries again after a while, a few times. */
constexpr uint32_t FAILED_MESSAGE_SHOW_MS = 10000;
constexpr uint32_t RETRY_AFTER_MS = 30000;
constexpr int MAX_RETRIES = 3;
constexpr uint32_t ENTER_RELEASE_MS = 150;
constexpr uint32_t GAP_POLL_MS = 300;
constexpr uint32_t SLOW_LOOP_MS = 150;
/** The message about missing detail comes back at most this often. */
constexpr uint32_t GAP_HINT_MS = 30000;
constexpr uint32_t LONG_PRESS_MS = 600;
constexpr const char* NORMAL_FONTS[] = { "text_default_", "mono_mono_", "text_small_" };
constexpr const char* SMALL_FONTS[] = { "text_small_" };
/** Text at least 12 px high: a smaller font is only used when its lines are this tall. */
constexpr int MIN_LINE_HEIGHT = 15;

} // namespace

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

bool MapView::begin(Device* display, const char* root, const char* dataPath) {
    // Idle time is counted from the start, not from boot
    lastInputAt = nowMs();
    snprintf(rootPath, sizeof(rootPath), "%s", root);
    if (!output.begin(display)) {
        logError("Display output failed");
        return false;
    }
    width = output.width();
    height = output.height();
    monochrome = output.isMonochrome();
    palette = monochrome ? &monochromePalette() : &colorPalette();
    theme = uiTheme(*palette, monochrome);
    frame = static_cast<uint16_t*>(largeAlloc(static_cast<size_t>(width) * height * sizeof(uint16_t)));
    preview = static_cast<uint16_t*>(largeAlloc(TileCache::TILE_BYTES));
    scratch = static_cast<AffineScratch*>(largeAlloc(sizeof(AffineScratch)));
    if (frame == nullptr || preview == nullptr || scratch == nullptr) {
        logError("Out of memory");
        return false;
    }
    canvas = { frame, width, height };
    // From here on only the I/O task touches the SD card
    if (!io.start()) {
        logError("I/O task not started");
        return false;
    }
    browsedCounter.configure(io, root);
    int regionsFound = 0;
    auto loadState = [&] {
        char settingsPath[PATH_MAX_LENGTH];
        snprintf(settingsPath, sizeof(settingsPath), "%s/settings.txt", root);
        readSettings(settingsPath, settings);
        snprintf(settingsPath, sizeof(settingsPath), "%s/view.txt", root);
        // The palette can be forced to monochrome, to check its contrast on a colour display
        if (settings.devMonochrome && !monochrome) {
            monochrome = true;
            palette = &monochromePalette();
            theme = uiTheme(*palette, monochrome);
        }
        hasSavedView = readSavedView(settingsPath, savedView);
        store.setRoot(root);
        regionsFound = store.scanRegions();
        char folder[160];
        snprintf(folder, sizeof(folder), "%s/cache/fonts", dataPath);
        if (!normalFont.borrow(FONT_SIZE_DEFAULT) && !normalFont.open(folder, NORMAL_FONTS, 3)) {
            logError("No font from the system or in %s, the map shows no text", folder);
        }
        if (!smallFont.borrow(FONT_SIZE_SMALL)) {
            smallFont.open(folder, SMALL_FONTS, 1);
        }
        largeFont.borrow(FONT_SIZE_LARGE);
        // The label fonts made on an earlier run; the ones missing are made in the background after the labels start
        for (int i = 0; i < LabelFontMaker::COUNT; i++) {
            char fontPath[200];
            LabelFontMaker::cachePath(root, i, fontPath, sizeof(fontPath));
            if (!fileExists(fontPath) || !labelFonts[i].openFile(fontPath)) {
                missingLabelFonts |= 1u << i;
            }
        }
        script.open(root);
        return true;
    };
    runOnCard(io, loadState);
    logInfo("Map root %s, %d regions", root, regionsFound);
    loadWaypoints();
    // Known before any download resumes: a region with a catalog outline needs it for its tile list
    app_paths_get_assets_path(APP_ID, "regions.bin", catalogPath, sizeof(catalogPath));
    downloader.setCatalogPath(catalogPath);
    TrackRecorder::closeUnfinished(io, rootPath);
    // GPX files are dropped into gpx/ at the top of the data volume (the SD card), where users find it
    char volume[PATH_MAX_LENGTH] = "";
    if (paths_get_data_root_path(volume, sizeof(volume)) == ERROR_NONE) {
        snprintf(gpxFolder, sizeof(gpxFolder), "%s/gpx", volume);
    } else {
        const char* second = strchr(rootPath + 1, '/');
        snprintf(gpxFolder, sizeof(gpxFolder), "%.*s/gpx", second != nullptr ? static_cast<int>(second - rootPath) : 0, rootPath);
    }
    logInfo("GPX folder %s", gpxFolder);
    loadShownGpx();
    overlay.setFonts(&normalFont, smallFont.isOpen() && smallFont.lineHeight() >= MIN_LINE_HEIGHT ? &smallFont : &normalFont);
    worker.setDeflatePrepared(settings.deflatePrepared);
    worker.setSdCache(settings.sdCache);
    // Labels: sections copied by the render lanes, placed by the map loop; without memory the map has none
    labelsReady = labelStore.begin() && labels.begin(largeAlloc, largeFree);
    if (labelsReady) {
        worker.setLabelStore(&labelStore);
        labels.setClock([] { return static_cast<uint32_t>(esp_timer_get_time()); });
        applyLabelFonts();
        applyLabelSettings();
    } else {
        logError("No memory for labels");
    }
    if (labelsReady && missingLabelFonts != 0) {
        char ttfPath[260];
        app_paths_get_assets_path(APP_ID, "fonts/LabelSans.ttf", ttfPath, sizeof(ttfPath));
        fontMaker.configure(io, root, ttfPath);
        fontMaker.start(missingLabelFonts);
    }
    if (!worker.start(&store, &io, palette, TILE_CACHE_SLOTS, root)) {
        return false;
    }
    input.begin();
    pointerAcceleration.setSpeed(settings.pointerSpeed);
    cursorX = width / 2;
    cursorY = height / 2;
    // The overlay adapts to the inputs the board has: key hints with a keyboard, focus without touch or pointer
    controls.setInputs({ input.hasKeyboard(), input.hasTouch(), input.hasPointer() });
    layoutControls();
    downloadView = downloadLayout(width, height, overlay.font().lineHeight());
    downloader.setTiming(settings.devProbe);
    downloader.setMemoryRelief([](void* context, size_t bytes) { return static_cast<TileWorker*>(context)->relieveMemory(bytes); }, &worker);
    if (!downloader.start(root, &store, &io)) {
        logError("Downloader not started");
    }
    routeProfile = static_cast<RouteProfile>(std::clamp(settings.routeProfile, 0, 2));
    navigatorStarted = navigator.start(io, store, worker, root);
    if (!navigatorStarted) {
        logError("Route search not started");
    }
    probe.watch(xTaskGetCurrentTaskHandle(), "main");
    probe.watch(worker.laneTask(0), "lane0");
    probe.watch(worker.laneTask(1), "lane1");
    probe.watch(io.taskHandle(), "io");
    probe.watch(downloader.controlTaskHandle(), "download");
    probe.watch(downloader.browseTaskHandle(), "browse");
    if (!(settings.devProbe ? probe.start() : probe.startWatchdog())) {
        logError("Stall probes not started");
    }

    if (hasSavedView) {
        centerOn(savedView.lat, savedView.lon, savedView.zoom);
    } else {
        centerOn(DEFAULT_LAT, DEFAULT_LON, DEFAULT_ZOOM);
    }
    viewReady = true;

    task_event_group_construct(&eventGroup);
    eventGroupReady = true;
    subscribed = app_event_subscribe(&appEvents, &eventGroup) == ERROR_NONE;
    location.subscribe(&eventGroup);
    readLocation();
    attribution.expand(nowMs());
    logMemory("map ready");
    return true;
}

void MapView::end() {
    if (viewReady) {
        saveView();
        viewReady = false;
    }
    // The track file gets its end tags; recording only runs while the app is open
    recorder.stop(io, nowMs());
    for (int wait = 0; gpxImport.state() == GpxImport::State::Busy && wait < 500; wait++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    gpxImport.acknowledge();
    for (int i = 0; i < gpxFileCount; i++) {
        if (gpxFiles[i].layer != nullptr) {
            gpxFiles[i].layer->~GpxLayer();
            largeFree(gpxFiles[i].layer);
            gpxFiles[i].layer = nullptr;
        }
    }
    fontMaker.stop();
    browsedCounter.stop();
    downloader.stop();
    if (navigatorStarted) {
        navigator.stop();
        navigatorStarted = false;
    }
    worker.stop();
    io.stop();
    probe.stop();
    location.unsubscribe();
    if (subscribed) {
        app_event_unsubscribe(&appEvents);
        subscribed = false;
    }
    if (eventGroupReady) {
        task_event_group_destruct(&eventGroup);
        eventGroupReady = false;
    }
    input.end();
    labels.end();
    labelStore.end();
    normalFont.close();
    smallFont.close();
    largeFont.close();
    for (BinFontText& font : labelFonts) {
        font.close();
    }
    output.end();
    largeFree(frame);
    frame = nullptr;
    largeFree(preview);
    preview = nullptr;
    largeFree(scratch);
    scratch = nullptr;
    largeFree(aboutBuffer);
    aboutBuffer = nullptr;
    for (int i = 0; i < squeezedCount; i++) {
        largeFree(squeezed[i]);
    }
    squeezedCount = 0;
}

void MapView::squeezeMemory(size_t largest) {
    // Blocks of 64 KB are taken until no free block is as large as asked, then every other one is given back: much
    // PSRAM stays free, but only in small pieces, like after a long session
    constexpr size_t BLOCK = 64 * 1024;
    int taken = 0;
    void* blocks[MAX_SQUEEZED * 2];
    while (taken < MAX_SQUEEZED * 2 && heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= largest) {
        void* block = largeAlloc(BLOCK);
        if (block == nullptr) {
            break;
        }
        blocks[taken++] = block;
    }
    for (int i = 0; i < taken; i++) {
        if (i % 2 == 0 || squeezedCount >= MAX_SQUEEZED) {
            largeFree(blocks[i]);
        } else {
            squeezed[squeezedCount++] = blocks[i];
        }
    }
    logInfo("debug: squeeze: %d blocks of 64 KB held, PSRAM %u KB free, largest block %u KB", squeezedCount,
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) / 1024));
}

void MapView::centerOn(double lat, double lon, int newZoom) {
    zoom = newZoom;
    centerX = lonToWorldX(lon, zoom);
    centerY = latToWorldY(lat, zoom);
    dirty = true;
    updateWanted();
}

void MapView::layoutControls() {
    controls.layout(width, height, overlay.scaleBarTop(canvas), navigating ? navBar.height(sheetFont()) : 0);
}

void MapView::centerOnPosition() {
    const PositionFix& fix = position.fix();
    zoom = std::clamp(zoom, 0, MAX_VIEW_ZOOM);
    centerX = lonToWorldX(fix.longitude, zoom);
    centerY = latToWorldY(fix.latitude, zoom);
    if (navigating) {
        // The position at NAVIGATION_POSITION_Y of the height: the screen centre is that far ahead of it, in the
        // direction that is up on the (turned) screen
        double aheadX;
        double aheadY;
        screenToWorldOffset(0.0, height / 2.0 - height * NAVIGATION_POSITION_Y, aheadX, aheadY);
        const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), zoom);
        centerX = std::fmod(centerX + aheadX + worldSize, worldSize);
        centerY = std::fmin(std::fmax(centerY + aheadY, 0.0), worldSize);
    }
    dirty = true;
    updateWanted();
}

void MapView::setZoom(int newZoom, int anchorX, int anchorY) {
    if (newZoom < 0 || newZoom > MAX_VIEW_ZOOM || newZoom == zoom) {
        return;
    }
    // The world point under the anchor stays where it is on the screen
    double offsetX;
    double offsetY;
    screenToWorldOffset(anchorX - width / 2.0, anchorY - height / 2.0, offsetX, offsetY);
    const double scale = std::ldexp(1.0, newZoom - zoom);
    centerX = (centerX + offsetX) * scale - offsetX;
    centerY = (centerY + offsetY) * scale - offsetY;
    zoom = newZoom;
    dirty = true;
    updateWanted();
}

void MapView::zoomBy(int steps, int anchorX, int anchorY) {
    const int target = std::min(std::max(zoom + steps, 0), MAX_VIEW_ZOOM);
    if (target == zoom) {
        return;
    }
    if (!smoothZoom()) {
        animation.active = false;
        io.hold(false);
        setZoom(target, anchorX, anchorY);
        return;
    }
    // Where the animation shows the map now: the current scale, or 1 when none runs. A second press during an
    // animation starts from what is on screen.
    const uint32_t now = nowMs();
    float scale = 1.0f;
    double anchorWorldX;
    double anchorWorldY;
    screenToWorldOffset(anchorX - width / 2.0, anchorY - height / 2.0, anchorWorldX, anchorWorldY);
    double worldX = centerX + anchorWorldX;
    double worldY = centerY + anchorWorldY;
    if (animation.active) {
        const float t = std::min(1.0f, static_cast<float>(now - animation.startMs) / ZOOM_ANIMATION_MS);
        const float eased = 1.0f - (1.0f - t) * (1.0f - t);
        scale = powf(animation.fromScale, 1.0f - eased);
        double movedX;
        double movedY;
        screenToWorldOffset((anchorX - animation.anchorX) / scale, (anchorY - animation.anchorY) / scale, movedX, movedY);
        worldX = animation.worldX + movedX;
        worldY = animation.worldY + movedY;
    }
    // The world point under the anchor stays there through the animation and after it
    const double factor = std::ldexp(1.0, target - zoom);
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), target);
    worldX *= factor;
    worldY *= factor;
    centerX = std::fmod(worldX - anchorWorldX + worldSize, worldSize);
    centerY = std::fmin(std::fmax(worldY - anchorWorldY, 0.0), worldSize);
    animation.active = true;
    io.hold(true);
    animation.startMs = now;
    animation.fromScale = scale / static_cast<float>(factor);
    animation.anchorX = static_cast<float>(anchorX);
    animation.anchorY = static_cast<float>(anchorY);
    animation.worldX = worldX;
    animation.worldY = worldY;
    animation.frames = 0;
    animation.composeMs = 0;
    animation.slowestFrameMs = 0;
    animation.slowestLockMs = 0;
    animation.slowestComposeMs = 0;
    zoom = target;
    dirty = true;
    // The tiles of the new level are asked for when the animation ends: their reads from the card hold the SPI bus
    // the T-Deck's display shares, which stalled animation frames by up to 240 ms
}

void MapView::screenToWorldOffset(double sx, double sy, double& wx, double& wy) const {
    wx = bearingCos * sx - bearingSin * sy;
    wy = bearingSin * sx + bearingCos * sy;
}

void MapView::worldToScreenOffset(double wx, double wy, double& sx, double& sy) const {
    sx = bearingCos * wx + bearingSin * wy;
    sy = -bearingSin * wx + bearingCos * wy;
}

void MapView::viewBounds(double& left, double& top, double& right, double& bottom) const {
    // The world box around the four turned screen corners
    const double halfWidth = width / 2.0;
    const double halfHeight = height / 2.0;
    const double spanX = std::fabs(bearingCos) * halfWidth + std::fabs(bearingSin) * halfHeight;
    const double spanY = std::fabs(bearingSin) * halfWidth + std::fabs(bearingCos) * halfHeight;
    left = centerX - spanX;
    right = centerX + spanX;
    top = centerY - spanY;
    bottom = centerY + spanY;
}

void MapView::applyBearing(float degrees, bool want) {
    degrees -= 360.0f * std::floor(degrees / 360.0f);
    // Within a tenth of a degree of north is north, so the fast unturned drawing comes back
    if (degrees < 0.1f || degrees > 359.9f) {
        degrees = 0.0f;
    }
    if (degrees == bearing) {
        return;
    }
    bearing = degrees;
    const float radians = degrees * 0.017453292f;
    bearingCos = degrees == 0.0f ? 1.0f : std::cos(radians);
    bearingSin = degrees == 0.0f ? 0.0f : std::sin(radians);
    dirty = true;
    if (want) {
        updateWanted();
    }
}

Rect MapView::compassRect() const {
    return { width - 6 - 30, 6, 30, 30 };
}

void MapView::drawCompass() {
    // A needle to north, red half towards it, in a round panel; only on a turned map
    const Rect r = compassRect();
    const float cx = r.x + r.width / 2.0f;
    const float cy = r.y + r.height / 2.0f;
    float shown = bearing;
    if (turn.active) {
        const float t = std::min(1.0f, static_cast<float>(nowMs() - turn.startMs) / TURN_ANIMATION_MS);
        shown = turn.from + (turn.to - turn.from) * (1.0f - (1.0f - t) * (1.0f - t));
    }
    const float radians = shown * 0.017453292f;
    // North on screen is the world's up turned back by the bearing
    const float northX = -std::sin(radians);
    const float northY = -std::cos(radians);
    canvas.fillCircle(cx, cy, 14.0f, theme.panel);
    canvas.ring(cx, cy, 14.0f, 1.5f, theme.text);
    canvas.line(cx, cy, cx + northX * 10.0f, cy + northY * 10.0f, 3.0f, monochrome ? theme.text : static_cast<uint16_t>(0xD8A3));
    canvas.line(cx, cy, cx - northX * 10.0f, cy - northY * 10.0f, 3.0f, monochrome ? theme.panel : theme.text);
    canvas.fillCircle(cx, cy, 2.0f, theme.text);
}

void MapView::setBearing(float degrees, bool animate) {
    // A turn already running continues from where it shows the map now
    float current = bearing;
    if (turn.active) {
        const float t = std::min(1.0f, static_cast<float>(nowMs() - turn.startMs) / TURN_ANIMATION_MS);
        current = turn.from + (turn.to - turn.from) * (1.0f - (1.0f - t) * (1.0f - t));
        turn.active = false;
        io.hold(false);
    }
    if (!animate || !smoothZoom()) {
        applyBearing(degrees);
        return;
    }
    // The shorter way round
    float target = degrees - 360.0f * std::floor(degrees / 360.0f);
    float delta = target - current;
    delta -= 360.0f * std::floor((delta + 180.0f) / 360.0f);
    if (std::fabs(delta) < 0.1f) {
        applyBearing(target);
        return;
    }
    turn.active = true;
    // Card work waits during the turn, its reads would hold the display's shared bus
    io.hold(true);
    turn.from = current;
    turn.to = current + delta;
    turn.startMs = nowMs();
    turn.frames = 0;
    turn.composeMs = 0;
    dirty = true;
}

void MapView::pan(double dx, double dy) {
    if (dx == 0.0 && dy == 0.0) {
        return;
    }
    following = false;
    headingUp = false;
    double worldDx;
    double worldDy;
    screenToWorldOffset(dx, dy, worldDx, worldDy);
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), zoom);
    centerX = std::fmod(centerX + worldDx + worldSize, worldSize);
    centerY = std::fmin(std::fmax(centerY + worldDy, 0.0), worldSize);
    dirty = true;
    updateWanted();
}

void MapView::updateWanted() {
    // The visible tiles first, nearest to the centre first, then a ring around them; a turned view needs the tiles
    // of the box around it (up to its 400 px diagonal on the T-Deck)
    double left;
    double top;
    double right;
    double bottom;
    viewBounds(left, top, right, bottom);
    const int firstX = static_cast<int>(std::floor(left / TILE_SIZE)) - 1;
    const int firstY = static_cast<int>(std::floor(top / TILE_SIZE)) - 1;
    const int lastX = static_cast<int>(std::floor((right - 1) / TILE_SIZE)) + 1;
    const int lastY = static_cast<int>(std::floor((bottom - 1) / TILE_SIZE)) + 1;
    TileKey tiles[TileWorker::MAX_WANTED];
    double distances[TileWorker::MAX_WANTED];
    int count = 0;
    const int tileCount = 1 << zoom;
    for (int y = firstY; y <= lastY; y++) {
        if (y < 0 || y >= tileCount) {
            continue;
        }
        for (int x = firstX; x <= lastX && count < TileWorker::MAX_WANTED; x++) {
            const double tileCenterX = (x + 0.5) * TILE_SIZE;
            const double tileCenterY = (y + 0.5) * TILE_SIZE;
            const bool visible = x > firstX && x < lastX && y > firstY && y < lastY;
            double distance = std::sqrt((tileCenterX - centerX) * (tileCenterX - centerX) + (tileCenterY - centerY) * (tileCenterY - centerY));
            if (!visible) {
                distance += 1e6;
            }
            int slot = count;
            while (slot > 0 && distances[slot - 1] > distance) {
                tiles[slot] = tiles[slot - 1];
                distances[slot] = distances[slot - 1];
                slot--;
            }
            tiles[slot] = { zoom, wrapTileX(x, zoom), y };
            distances[slot] = distance;
            count++;
        }
    }
    int visibleCount = 0;
    while (visibleCount < count && distances[visibleCount] < 1e6) {
        visibleCount++;
    }
    worker.want(tiles, count, visibleCount);
    // Detail asked for an earlier view is not fetched any more once the view has moved on from it
    const int sourceZoom = std::min(zoom, MAX_SOURCE_ZOOM);
    const int shift = zoom - sourceZoom;
    // A view as wide as the world keeps every column
    const bool allColumns = lastX - firstX + 1 >= tileCount;
    downloader.keepBrowse(sourceZoom, allColumns ? 0 : wrapTileX(firstX, zoom) >> shift, std::max(firstY, 0) >> shift,
        allColumns ? (1 << sourceZoom) - 1 : wrapTileX(lastX, zoom) >> shift, std::min(lastY, tileCount - 1) >> shift);
    downloader.setView({ worldXToLon(left, zoom), worldYToLat(bottom, zoom), worldXToLon(right, zoom), worldYToLat(top, zoom) });
    viewChangedAt = nowMs();
    viewTimed = false;
}

const uint16_t* MapView::lookupTile(const TileKey& tile, void* context) {
    return static_cast<MapView*>(context)->findTile(tile);
}

const uint16_t* MapView::lookupCached(const TileKey& tile, void* context) {
    return static_cast<MapView*>(context)->worker.cache().find(tile);
}

const uint16_t* MapView::findTile(const TileKey& tile) {
    TileCache& cache = worker.cache();
    if (const uint16_t* pixels = cache.find(tile)) {
        return pixels;
    }
    // A magnified part of a cached ancestor stands in until the tile is ready
    for (int level = 1; level <= 4 && tile.z - level >= 0; level++) {
        const TileKey parent = ancestorOf(tile, tile.z - level);
        const uint16_t* pixels = cache.find(parent);
        if (pixels == nullptr) {
            continue;
        }
        const int size = TILE_SIZE >> level;
        const int originX = (tile.x - (parent.x << level)) * size;
        const int originY = (tile.y - (parent.y << level)) * size;
        for (int y = 0; y < TILE_SIZE; y++) {
            const uint16_t* source = pixels + (originY + (y >> level)) * TILE_SIZE + originX;
            uint16_t* target = preview + y * TILE_SIZE;
            for (int x = 0; x < TILE_SIZE; x++) {
                target[x] = source[x >> level];
            }
        }
        return preview;
    }
    // After zooming out, the finer tiles that were just on screen stand in, shrunk, instead of an empty screen
    for (int level = 1; level <= 2 && tile.z + level <= MAX_VIEW_ZOOM; level++) {
        const int span = 1 << level;
        const int size = TILE_SIZE >> level;
        bool any = false;
        for (int childY = 0; childY < span; childY++) {
            for (int childX = 0; childX < span; childX++) {
                const TileKey child { tile.z + level, (tile.x << level) + childX, (tile.y << level) + childY };
                const uint16_t* pixels = cache.find(child);
                for (int y = 0; y < size; y++) {
                    uint16_t* target = preview + (childY * size + y) * TILE_SIZE + childX * size;
                    if (pixels == nullptr) {
                        for (int x = 0; x < size; x++) {
                            target[x] = palette->placeholder;
                        }
                        continue;
                    }
                    // Average of two pixels on a row: halves aliasing for little work
                    const uint16_t* source = pixels + (y << level) * TILE_SIZE;
                    for (int x = 0; x < size; x++) {
                        const uint16_t a = source[x << level];
                        const uint16_t b = source[(x << level) + 1];
                        target[x] = static_cast<uint16_t>(((a & 0xF7DE) >> 1) + ((b & 0xF7DE) >> 1) + (a & b & 0x0821));
                    }
                }
                any = any || pixels != nullptr;
            }
        }
        if (any) {
            return preview;
        }
    }
    return nullptr;
}

bool MapView::isViewComplete() {
    double left;
    double top;
    double right;
    double bottom;
    viewBounds(left, top, right, bottom);
    const int tileCount = 1 << zoom;
    worker.lock();
    bool complete = true;
    for (int y = static_cast<int>(std::floor(top / TILE_SIZE)); y <= static_cast<int>(std::floor((bottom - 1) / TILE_SIZE)) && complete; y++) {
        for (int x = static_cast<int>(std::floor(left / TILE_SIZE)); x <= static_cast<int>(std::floor((right - 1) / TILE_SIZE)) && complete; x++) {
            if (y < 0 || y >= tileCount) {
                continue;
            }
            const TileKey tile = { zoom, wrapTileX(x, zoom), y };
            complete = worker.isSettled(tile);
        }
    }
    worker.unlock();
    return complete;
}

bool MapView::markerScreenPosition(int& x, int& y) const {
    if (!position.hasFix()) {
        return false;
    }
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), zoom);
    double dx = lonToWorldX(position.fix().longitude, zoom) - centerX;
    if (dx > worldSize / 2) {
        dx -= worldSize;
    } else if (dx < -worldSize / 2) {
        dx += worldSize;
    }
    double sx;
    double sy;
    worldToScreenOffset(dx, latToWorldY(position.fix().latitude, zoom) - centerY, sx, sy);
    x = static_cast<int>(std::floor(width / 2.0 + sx + 0.5));
    y = static_cast<int>(std::floor(height / 2.0 + sy + 0.5));
    // Far outside the screen only the accuracy circle can still show
    const int reach = 4 * (width + height);
    return x > -reach && y > -reach && x < width + reach && y < height + reach;
}

void MapView::drawStatus() {
    char text[96];
    const Downloader::Status& status = lastStatus;
    pillRect = {};
    statusShown = false;
    bool progress = true;
    if (status.state == Downloader::State::Downloading && status.total > 0) {
        formatProgressPill(TransferKind::Download, statusTitle, status.done, status.total, text, sizeof(text));
    } else if (status.state == Downloader::State::Updating && status.total > 0) {
        formatProgressPill(TransferKind::Update, statusTitle, status.done, status.total, text, sizeof(text));
    } else if (downloader.isBrowsing() && browseTotal > 0) {
        formatProgressPill(TransferKind::Browse, "", std::min(browseDone + 1, browseTotal), browseTotal, text, sizeof(text));
    } else if (downloader.isBrowsing()) {
        snprintf(text, sizeof(text), "Downloading map");
    } else if (status.state == Downloader::State::Connecting || status.state == Downloader::State::Deleting) {
        snprintf(text, sizeof(text), "%s", status.message);
    } else if (static_cast<int32_t>(nowMs() - messageUntil) < 0) {
        snprintf(text, sizeof(text), "%s", status.message);
    } else if (store.regionCount() == 0) {
        snprintf(text, sizeof(text), "No maps yet: Menu, Download map");
        progress = false;
    } else {
        return;
    }
    const Rect rect = overlay.drawPill(canvas, text, *palette);
    statusShown = true;
    if (progress) {
        pillRect = rect;
    }
}

void MapView::openDownloadCard() {
    downloadCard.open();
    fillDownloadCard();
    controls.hide();
    dirty = true;
}

void MapView::fillDownloadCard() {
    const Downloader::Status& status = lastStatus;
    const bool running = status.state == Downloader::State::Downloading || status.state == Downloader::State::Updating ||
        status.state == Downloader::State::Connecting;
    char lines[5][64];
    const char* rows[5];
    int count = 0;
    auto add = [&](const char* format, auto... values) {
        snprintf(lines[count], sizeof(lines[count]), format, values...);
        rows[count] = lines[count];
        count++;
    };
    if (running && status.total > 0) {
        add("%s %d%%, %d of %d tiles", status.state == Downloader::State::Updating ? "Updating" : "Downloading",
            static_cast<int>(status.done * 100 / status.total), static_cast<int>(status.done), static_cast<int>(status.total));
        // The size so far against an estimate from the tiles done
        char done[16];
        char estimate[16];
        formatBytes(status.bytes, done, sizeof(done));
        const uint64_t total = status.done > 0 ? status.bytes * static_cast<uint64_t>(status.total) / static_cast<uint64_t>(status.done) : 0;
        formatBytes(total, estimate, sizeof(estimate));
        add("%s of about %s", done, status.done > 0 ? estimate : "?");
        const double rate = transferRate.bytesPerSecond();
        if (rate > 0.0) {
            char speed[16];
            formatBytes(static_cast<uint64_t>(rate), speed, sizeof(speed));
            const int64_t left = transferRate.secondsLeft(status.bytes, total);
            char remaining[24] = "";
            if (left >= 0) {
                formatDuration(left, remaining, sizeof(remaining));
            }
            add("%s/s%s%s", speed, left >= 0 ? ", " : "", left >= 0 ? remaining : "");
            if (left >= 0) {
                snprintf(lines[count - 1] + strlen(lines[count - 1]), sizeof(lines[0]) - strlen(lines[count - 1]), " left");
            }
        } else {
            add("Measuring the speed...");
        }
    } else if (status.state == Downloader::State::Connecting) {
        add("%s", status.message);
    } else if (downloader.isBrowsing() && browseTotal > 0) {
        add("Fetching detail for the screen:");
        add("%d of %d tiles", std::min(browseDone + 1, browseTotal), browseTotal);
        add("They are kept in Downloaded maps,");
        add("region \"browsed\".");
    } else if (pausedRegion[0] != '\0') {
        add("Paused. Resume continues where it stopped.");
    } else {
        add("%s", status.message[0] != '\0' ? status.message : "Nothing is downloading.");
    }
    // Tiles fetched while browsing can't be paused or cancelled from the card, only the card closed
    const bool browsing = !running && status.state != Downloader::State::Connecting && downloader.isBrowsing() && browseTotal > 0;
    const char* title = browsing ? "Map detail" : statusTitle[0] != '\0' ? statusTitle : status.region;
    downloadCard.set(title, rows, count, !running, browsing);
    downloadCard.layout(width, height, overlay.font());
}

void MapView::handleCardButton(DownloadCard::Button button) {
    dirty = true;
    switch (button) {
        case DownloadCard::Button::PauseResume: {
            const Downloader::Status status = downloader.status();
            const bool running = status.state == Downloader::State::Downloading || status.state == Downloader::State::Connecting;
            if (running) {
                snprintf(pausedRegion, sizeof(pausedRegion), "%s", status.region);
                downloader.cancel();
                showToast("Download paused");
            } else if (status.state == Downloader::State::Updating) {
                downloader.cancel();
                showToast("Update paused, it continues in idle time");
            } else if (pausedRegion[0] != '\0') {
                downloader.download(pausedRegion);
                pausedRegion[0] = '\0';
                showToast("Download continues");
            }
            downloadCard.close();
            break;
        }
        case DownloadCard::Button::Cancel: {
            // Cancelling a download deletes the part already fetched, after asking
            const Downloader::Status status = downloader.status();
            const char* region = status.region[0] != '\0' ? status.region : pausedRegion;
            downloadCard.close();
            if (region[0] == '\0' || status.state == Downloader::State::Updating) {
                downloader.cancel();
                break;
            }
            snprintf(cancelRegion, sizeof(cancelRegion), "%s", region);
            char message[96];
            snprintf(message, sizeof(message), "Stop and delete the part of %s downloaded so far?", statusTitle[0] != '\0' ? statusTitle : region);
            dialogPurpose = DialogPurpose::CancelDownload;
            dialog.open(message, "Delete", true);
            dialog.layout(width, height, overlay.font());
            break;
        }
        case DownloadCard::Button::Close:
            downloadCard.close();
            break;
        default:
            break;
    }
}

void MapView::pollDownloader(uint32_t now) {
    TileKey arrived[Downloader::MAX_ARRIVED];
    int arrivedCount = 0;
    if (downloader.takeTilesChanged(arrived, arrivedCount)) {
        worker.tilesArrived(arrived, arrivedCount);
        dirty = true;
    }
    if (!resumeChecked) {
        // A download that was interrupted continues when the map opens again
        resumeChecked = true;
        int unfinished = -1;
        RegionMeta meta {};
        auto find = [&] {
            for (int i = 0; i < store.regionCount() && unfinished < 0; i++) {
                if (strcmp(store.regionName(i), pausedRegion) == 0) {
                    continue;
                }
                char path[PATH_MAX_LENGTH];
                snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, store.regionName(i));
                if (readRegionMeta(path, meta) && !meta.isComplete()) {
                    unfinished = i;
                }
            }
            return true;
        };
        runOnCard(io, find, IoPriority::Background);
        if (unfinished >= 0) {
            logInfo("Continuing the download of %s at tile %d of %d", store.regionName(unfinished), static_cast<int>(meta.done), static_cast<int>(meta.total));
            downloader.download(store.regionName(unfinished));
        }
    }
    if (retryAt != 0 && static_cast<int32_t>(now - retryAt) >= 0) {
        retryAt = 0;
        resumeChecked = false;
    }
    if (now - lastStatusPoll < STATUS_POLL_MS) {
        return;
    }
    lastStatusPoll = now;
    const Downloader::Status status = downloader.status();
    // Tile cache slots given to a large download come back once nothing downloads
    if (status.state != Downloader::State::Connecting && status.state != Downloader::State::Downloading &&
        status.state != Downloader::State::Updating && !downloader.isBrowsing() && worker.cacheShrunk() && !worker.isPreparing() &&
        navigator.state() != Navigator::State::Searching) {
        worker.restoreMemory();
    }
    downloader.browseProgress(browseDone, browseTotal);
    if (status.region[0] != '\0' && strcmp(status.region, statusTitleOf) != 0) {
        // The title users gave the region, not its short folder name
        snprintf(statusTitleOf, sizeof(statusTitleOf), "%s", status.region);
        snprintf(statusTitle, sizeof(statusTitle), "%s", status.region);
        RegionMeta meta {};
        char path[PATH_MAX_LENGTH];
        snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, status.region);
        auto read = [&] { return readRegionMeta(path, meta); };
        if (runOnCard(io, read, IoPriority::Background)) {
            snprintf(statusTitle, sizeof(statusTitle), "%s", regionTitle(meta));
        }
    }
    if (status.state == Downloader::State::Downloading || status.state == Downloader::State::Updating) {
        transferRate.add(now, status.bytes);
    } else {
        transferRate.reset();
    }
    // The pill follows browsing too, and goes when it ends
    const bool browsingNow = downloader.isBrowsing();
    if (browsingNow != lastBrowsing || browseDone != lastBrowseDone || browseTotal != lastBrowseTotal) {
        lastBrowsing = browsingNow;
        lastBrowseDone = browseDone;
        lastBrowseTotal = browseTotal;
        dirty = true;
    }
    worker.setConversionsPaused(status.state == Downloader::State::Connecting || status.state == Downloader::State::Downloading ||
        status.state == Downloader::State::Updating);
    if (status.state != lastStatus.state || status.done != lastStatus.done || strcmp(status.message, lastStatus.message) != 0) {
        if (status.state != lastStatus.state && (status.state == Downloader::State::Done || status.state == Downloader::State::Failed)) {
            messageUntil = now + (status.state == Downloader::State::Failed ? FAILED_MESSAGE_SHOW_MS : MESSAGE_SHOW_MS);
        }
        // A download that stopped on an error (the network, or memory) continues by itself a little later
        if (status.state == Downloader::State::Failed && (lastStatus.state == Downloader::State::Downloading ||
            lastStatus.state == Downloader::State::Connecting) && strstr(status.message, "limit") == nullptr) {
            if (retries < MAX_RETRIES) {
                retries++;
                retryAt = now + RETRY_AFTER_MS;
                logInfo("Download of %s failed, trying again in %u s (%d of %d)", status.region, RETRY_AFTER_MS / 1000, retries, MAX_RETRIES);
            }
        } else if (status.state == Downloader::State::Done && lastStatus.state == Downloader::State::Downloading) {
            retries = 0;
        }
        // After a finished download the next unfinished region continues, but not one paused on purpose
        if (status.state == Downloader::State::Done && lastStatus.state == Downloader::State::Downloading) {
            resumeChecked = false;
        }
        dirty = true;
        // The Regions screen shows progress as it happens
        if (screen == Screen::Regions && status.state == Downloader::State::Done) {
            lastStatus = status;
            openRegions();
        }
    }
    lastStatus = status;
    if (downloadCard.isOpen()) {
        fillDownloadCard();
        dirty = true;
    }
    if (static_cast<int32_t>(now - messageUntil) >= 0 && messageUntil != 0) {
        messageUntil = 0;
        dirty = true;
    }
}

void MapView::pollGaps(uint32_t now) {
    if (now - lastGapPoll < GAP_POLL_MS) {
        return;
    }
    lastGapPoll = now;
    TileKey gaps[8];
    const int count = worker.takeGaps(gaps, 8);
    if (count == 0) {
        return;
    }
    if (settings.browseDownload && Downloader::isOnline()) {
        downloader.browse(gaps, count);
        dirty = true;
        return;
    }
    if (lastGapHint != 0 && now - lastGapHint < GAP_HINT_MS) {
        return;
    }
    lastGapHint = now;
    if (!settings.browseDownload) {
        showToast("Less detail here. Download this area for more.");
    } else {
        showToast("Offline: no detail here. Download this area on Wi-Fi.");
    }
}

Rect MapView::attributionMark() const {
    if (screen == Screen::Download) {
        return downloadView.mark;
    }
    return { 0, height - MapOverlay::MARK_SIZE, MapOverlay::MARK_SIZE, MapOverlay::MARK_SIZE };
}

PositionButton MapView::positionButton() const {
    if (!position.hasFix()) {
        return PositionButton::Unavailable;
    }
    if (!following) {
        return PositionButton::Free;
    }
    return headingUp ? PositionButton::FollowingHeading : PositionButton::Following;
}

void MapView::drawMap() {
    const uint32_t start = nowMs();
    probe.phase("map lock");
    worker.lock();
    const uint32_t locked = nowMs();
    probe.phase("compose");
    if (isRotated()) {
        // Turned: every pixel through the transform, tiles not ready stood in for by cached ancestors or children;
        // nearest pixel while it moves, bilinear once it settled
        const bool bilinear = isSmoothView();
        composeAffine(frame, width, height, scaledView(centerX, centerY, zoom, 1.0f, width / 2.0f, height / 2.0f, width, height, bearingCos, bearingSin),
            lookupCached, this, palette->placeholder, *scratch, bilinear);
        if (bilinear && !smooth.logged) {
            smooth.logged = true;
            logInfo("Bilinear frame %u ms (z%d, %d degrees)", static_cast<unsigned>(nowMs() - locked), zoom, static_cast<int>(bearing));
        }
    } else {
        composeView(frame, width, height, centerX, centerY, zoom, lookupTile, this, palette->placeholder);
    }
    worker.unlock();
    frameLockMs = locked - start;
    frameComposeMs = nowMs() - locked;
    drawRoute();
    // Labels placed for this zoom and bearing move with the pixels while panning, until they are placed again
    if (labelsReady && labelZoom == zoom && labelBearing == bearing && labels.count() > 0) {
        double moveX;
        double moveY;
        worldToScreenOffset(labelCenterX - centerX, labelCenterY - centerY, moveX, moveY);
        const auto offsetX = static_cast<int>(std::floor(moveX + 0.5));
        const auto offsetY = static_cast<int>(std::floor(moveY + 0.5));
        probe.phase("label draw");
        labels.draw(canvas, offsetX, offsetY, true);
        probe.phase("overlays");
    }
    int markerX;
    int markerY;
    drawPlaces();
    if (markerScreenPosition(markerX, markerY)) {
        if (position.state() == PositionState::Approximate) {
            overlay.drawApproximateMarker(canvas, markerX, markerY, position.fix().accuracy, *palette);
        } else {
            const float radius = position.accuracyRadius(metresPerPixel(position.fix().latitude, zoom));
            overlay.drawAccuracyCircle(canvas, markerX, markerY, radius, *palette);
            overlay.drawMarker(canvas, markerX, markerY, position.showsArrow(), position.fix().course - bearing, *palette);
        }
    }
}

void MapView::applyLabelFonts() {
    LabelFont* small = labelFonts[0].isOpen() ? &labelFonts[0] : (smallFont.isOpen() ? &smallFont : &normalFont);
    LabelFont* normal = labelFonts[1].isOpen() ? &labelFonts[1] : static_cast<LabelFont*>(&normalFont);
    LabelFont* large = labelFonts[2].isOpen() ? &labelFonts[2] : (largeFont.isOpen() ? &largeFont : normal);
    labels.setFonts(small, normal, large);
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
        monochrome ? palette->halo : static_cast<uint16_t>(0xFFFF), binary, monochrome ? palette->text : static_cast<uint16_t>(0x7BEF) });
    labelZoom = -1;
}

void MapView::placeLabels() {
    const uint32_t start = nowMs();
    labels.beginView(width, height, zoom);
    // House numbers show from z17 (z18 with few labels): only then are they read from the label files
    labelStore.setNumbersWanted(zoom >= 17 + (settings.labels == static_cast<int>(LabelDensity::Few) ? 1 : 0));
    double left;
    double top;
    double right;
    double bottom;
    viewBounds(left, top, right, bottom);
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

void MapView::updateWork(uint32_t now) {
    const uint32_t before = work.generation();
    auto set = [&](WorkKind kind, bool running, int32_t done, int32_t total) {
        if (running && !work.isRunning(kind)) {
            work.start(kind, total);
        }
        if (running) {
            work.progress(kind, done, total);
        } else if (work.isRunning(kind)) {
            work.finish(kind);
        }
    };
    int prepared = 0;
    int waiting = 0;
    int splitting = 0;
    worker.preparation(prepared, waiting, splitting);
    // Older tiles with labels inside are split into their new files: shown as such while they are the most of the queue
    const bool split = splitting > 0 && splitting * 2 >= waiting;
    set(WorkKind::SplitTiles, waiting > 0 && split, prepared, prepared + waiting);
    set(WorkKind::PrepareTiles, waiting > 0 && !split, prepared, prepared + waiting);
    set(WorkKind::Fonts, labelsReady && missingLabelFonts != 0 && !labelFontsApplied, 0, 0);
    set(WorkKind::IdleUpdate, lastStatus.state == Downloader::State::Updating, static_cast<int32_t>(lastStatus.done),
        static_cast<int32_t>(lastStatus.total));
    set(WorkKind::GpxImport, gpxImport.state() == GpxImport::State::Busy, 0, 0);
    const int moved = downloader.movedTiles();
    set(WorkKind::MoveBrowsed, lastStatus.state == Downloader::State::Downloading && moved > 0, moved, 0);
    if (work.generation() != before) {
        dirty = true;
    }
    // The open card follows the counts
    if (workCard.isOpen() && now - lastWorkPulse >= 500) {
        lastWorkPulse = now;
        dirty = true;
    }
}

void MapView::drawWorkDot() {
    // Amber, not red: red is track recording. Drawn only with full frames, so menus and their dimming cover it
    ::maps::drawWorkDot(canvas, WORK_DOT_X, WORK_DOT_Y, monochrome ? palette->text : static_cast<uint16_t>(0xFD20));
}

bool MapView::wantsTouchKeyboard() const {
    if (input.hasKeyboard() || !input.hasTouch()) {
        return false;
    }
    return prompt.isOpen() || (sheet.isOpen() && sheet.mode() == WaypointSheet::Mode::New && sheet.isEditable());
}

void MapView::syncTouchKeyboard() {
    const bool wanted = wantsTouchKeyboard();
    if (wanted && !touchKeyboard.isOpen()) {
        touchKeyboard.open();
    } else if (!wanted && touchKeyboard.isOpen()) {
        touchKeyboard.close();
    }
    if (!touchKeyboard.isOpen()) {
        return;
    }
    // The text field moves up above the keyboard
    const int above = height - TouchKeyboard::heightFor(width);
    touchKeyboard.layout(width, height, sheetFont());
    if (prompt.isOpen()) {
        prompt.layout(width, above, overlay.font());
    }
    if (sheet.isOpen()) {
        sheet.layout(width, above, sheetFont(), true);
    }
}

bool MapView::isSmoothView() const {
    return smooth.valid && smooth.zoom == zoom && smooth.bearing == bearing && std::fabs(smooth.centerX - centerX) < 0.01 &&
        std::fabs(smooth.centerY - centerY) < 0.01;
}

void MapView::updateSmooth(uint32_t now) {
    if (!isRotated() || isSmoothView() || animation.active || turn.active || pinching || isFullScreen() || now - lastInputAt < 250 ||
        !isViewComplete()) {
        return;
    }
    smooth = { true, centerX, centerY, zoom, bearing, false };
    dirty = true;
}

void MapView::drawScaledMap() {
    const uint32_t start = nowMs();
    AffineView view {};
    float scale = 1.0f;
    double worldX = 0.0;
    double worldY = 0.0;
    float anchorX = 0.0f;
    float anchorY = 0.0f;
    if (pinching) {
        // Drawn from the nearest whole zoom level, so the cached tiles are never scaled by more than 1.4 either way
        const int base = snapZoom(zoom, pinchScale, MAX_VIEW_ZOOM);
        const auto factor = static_cast<float>(std::ldexp(1.0, base - zoom));
        scale = pinchScale / factor;
        worldX = pinchWorldX * factor;
        worldY = pinchWorldY * factor;
        anchorX = static_cast<float>(pinchX);
        anchorY = static_cast<float>(pinchY);
        view = scaledView(worldX, worldY, base, scale, anchorX, anchorY, width, height, bearingCos, bearingSin);
    } else if (turn.active) {
        // Turning around the screen centre at the current zoom
        const float t = std::min(1.0f, static_cast<float>(start - turn.startMs) / TURN_ANIMATION_MS);
        const float eased = 1.0f - (1.0f - t) * (1.0f - t);
        const float radians = (turn.from + (turn.to - turn.from) * eased) * 0.017453292f;
        view = scaledView(centerX, centerY, zoom, 1.0f, width / 2.0f, height / 2.0f, width, height, std::cos(radians), std::sin(radians));
    } else {
        const float t = std::min(1.0f, static_cast<float>(start - animation.startMs) / ZOOM_ANIMATION_MS);
        const float eased = 1.0f - (1.0f - t) * (1.0f - t);
        scale = powf(animation.fromScale, 1.0f - eased);
        worldX = animation.worldX;
        worldY = animation.worldY;
        anchorX = animation.anchorX;
        anchorY = animation.anchorY;
        view = scaledView(worldX, worldY, zoom, scale, anchorX, anchorY, width, height, bearingCos, bearingSin);
    }
    worker.lock();
    const uint32_t locked = nowMs();
    composeAffine(frame, width, height, view, lookupCached, this, palette->placeholder, *scratch);
    worker.unlock();
    const uint32_t composed = nowMs();
    animation.frameLockMs = locked - start;
    animation.frameComposeMs = composed - locked;
    if (turn.active) {
        turn.frames++;
        turn.composeMs += composed - start;
    } else if (!pinching) {
        animation.frames++;
        animation.composeMs += composed - start;
    }
    int markerX;
    int markerY;
    if (!pinching && !turn.active && markerScreenPosition(markerX, markerY)) {
        // The marker moves with the map: its offset from the anchor scales like the map does; turned offsets need no
        // change, the zoom animation keeps the bearing
        const float x = anchorX + (static_cast<float>(markerX) - anchorX) * scale;
        const float y = anchorY + (static_cast<float>(markerY) - anchorY) * scale;
        overlay.drawMarker(canvas, static_cast<int>(x), static_cast<int>(y), position.showsArrow(), position.fix().course - bearing, *palette);
    }
}

void MapView::render() {
    const uint32_t start = nowMs();
    syncTouchKeyboard();
    probe.phase("render");
    if (animation.active && (pinching || isFullScreen() || start - animation.startMs >= ZOOM_ANIMATION_MS)) {
        animation.active = false;
        io.hold(false);
        updateWanted();
        logInfo("Zoom animation: %d frames in %u ms, compose %u ms per frame, slowest frame %u ms (lock %u, compose %u, rest display)",
            animation.frames, static_cast<unsigned>(start - animation.startMs),
            animation.frames > 0 ? static_cast<unsigned>(animation.composeMs / animation.frames) : 0, static_cast<unsigned>(animation.slowestFrameMs),
            static_cast<unsigned>(animation.slowestLockMs), static_cast<unsigned>(animation.slowestComposeMs));
    }
    if (turn.active && (pinching || isFullScreen() || start - turn.startMs >= TURN_ANIMATION_MS)) {
        turn.active = false;
        io.hold(false);
        logInfo("Turn to %d degrees: %d frames in %u ms, compose %u ms per frame", static_cast<int>(turn.to), turn.frames,
            static_cast<unsigned>(start - turn.startMs), turn.frames > 0 ? static_cast<unsigned>(turn.composeMs / turn.frames) : 0);
        applyBearing(turn.to);
    }
    if (pinching || animation.active || turn.active) {
        // The cached tiles scaled live, sharp tiles follow when the animation ends or the fingers lift
        drawScaledMap();
        if (screen == Screen::Download) {
            // The frame and its bar stay put while the map scales under them
            overlay.setBottomInset(downloadView.bar.height);
            drawDownloadFrame();
        } else {
            overlay.setBottomInset(0);
        }
        overlay.drawAttributionMark(canvas, *palette);
        output.present(frame, 0, height);
        if (animation.active || turn.active) {
            if (animation.active && nowMs() - start > animation.slowestFrameMs) {
                animation.slowestFrameMs = nowMs() - start;
                animation.slowestLockMs = animation.frameLockMs;
                animation.slowestComposeMs = animation.frameComposeMs;
            }
            // The next frame comes with the next loop pass
            dirty = true;
        } else {
            dirty = false;
        }
        return;
    }
    if (isFullScreen()) {
        if (screen == Screen::About) {
            drawAbout();
        } else {
            activeList()->draw(canvas, overlay.font(), theme);
        }
    } else {
        drawMap();
        overlay.setBottomInset(screen == Screen::Download ? downloadView.bar.height : 0);
        if (screen == Screen::Map) {
            const double lat = worldYToLat(centerY, zoom);
            overlay.drawScaleBar(canvas, metresPerPixel(lat, zoom), *palette);
        }
        // The full attribution shows for a few seconds, then collapses into a mark that opens it again
        if (screen == Screen::Download) {
            drawDownloadFrame();
        }
        if (attribution.isExpanded()) {
            overlay.drawAttribution(canvas, *palette, attributionBar.x, attributionBar.y, attributionBar.width, attributionBar.height);
        } else if (screen == Screen::Map) {
            overlay.drawAttributionMark(canvas, *palette);
        }
        if (screen == Screen::Map) {
            if (isRotated() || turn.active) {
                drawCompass();
            }
            controls.draw(canvas, overlay.font(), theme, positionButton());
            if (controls.isVisible() && !sheet.isOpen() && !cursorShown) {
                // The crosshair Enter picks places from: dark with a light rim, like the pointer
                const auto x = static_cast<float>(width / 2);
                const auto y = static_cast<float>(height / 2);
                canvas.line(x - 7.0f, y, x + 7.0f, y, 3.0f, theme.panel);
                canvas.line(x, y - 7.0f, x, y + 7.0f, 3.0f, theme.panel);
                canvas.line(x - 6.0f, y, x + 6.0f, y, 1.5f, theme.text);
                canvas.line(x, y - 6.0f, x, y + 6.0f, 1.5f, theme.text);
            }
            if (sheet.isOpen()) {
                sheet.draw(canvas, sheetFont(), theme);
            }
            if (routeCard.isOpen()) {
                routeCard.draw(canvas, sheetFont(), theme);
            }
            if (navigating) {
                navBar.draw(canvas, sheetFont(), theme, width);
            }
            if (pickingStart && !menuOpen) {
                drawPickStart();
            }
        }
        if (screen == Screen::Map) {
            drawStatus();
            drawRecording();
        }
        workDotShown = screen == Screen::Map && work.any() && !statusShown && !controls.isVisible() && !sheet.isOpen() && !navigating;
        if (workDotShown) {
            drawWorkDot();
        }
    }
    if (menuOpen) {
        menu.draw(canvas, overlay.font(), theme);
    }
    if (dialog.isOpen()) {
        dialog.draw(canvas, overlay.font(), theme);
    }
    syncTouchKeyboard();
    if (prompt.isOpen()) {
        prompt.draw(canvas, overlay.font(), theme);
    }
    if (touchKeyboard.isOpen()) {
        touchKeyboard.draw(canvas, sheetFont(), theme);
    }
    if (downloadCard.isOpen() && !dialog.isOpen()) {
        downloadCard.draw(canvas, overlay.font(), theme);
    }
    if (workCard.isOpen() && !dialog.isOpen()) {
        workCard.set(work);
        workCard.layout(width, height, overlay.font());
        workCard.draw(canvas, overlay.font(), theme);
    }
    if (static_cast<int32_t>(nowMs() - toastUntil) < 0) {
        overlay.drawToast(canvas, toastText, theme);
    }
    if (cursorShown && input.hasPointer()) {
        // An arrow pointer, dark with a light rim so it shows on any map and screen
        const auto x = static_cast<float>(cursorX);
        const auto y = static_cast<float>(cursorY);
        canvas.line(x, y, x, y + 13.0f, 4.0f, theme.panel);
        canvas.line(x, y, x + 9.0f, y + 9.0f, 4.0f, theme.panel);
        canvas.line(x, y, x, y + 13.0f, 2.0f, theme.text);
        canvas.line(x, y, x + 9.0f, y + 9.0f, 2.0f, theme.text);
    }
    const uint32_t presentStart = nowMs();
    probe.phase("display");
    output.present(frame, 0, height);
    probe.phase("after display");
    dirty = false;
    const uint32_t frameEnd = nowMs();
    if (frameEnd - start > SLOW_LOOP_MS) {
        logInfo("Slow frame %u ms: lock %u, compose %u, overlays %u, display %u", frameEnd - start, frameLockMs, frameComposeMs,
            presentStart - start - frameLockMs - frameComposeMs, frameEnd - presentStart);
    }
    frameLockMs = 0;
    frameComposeMs = 0;
    if (!viewTimed && !isFullScreen() && isViewComplete()) {
        viewTimed = true;
        // The first connection to OpenFreeMap stalls the device for about 1.5 s: after the first view, not during it
        if (!browsingPrepared && settings.browseDownload) {
            browsingPrepared = true;
            downloader.prepareBrowsing();
        }
        logInfo("View z%d complete %u ms after the change (frame %u ms)", zoom, nowMs() - viewChangedAt, nowMs() - start);
        logMemory("view complete");
        // How close each task came to the end of its stack, so stack sizes can be set from measurements
        logInfo("Stack left: main %u, io %u, render0 %u, render1 %u bytes", static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
            static_cast<unsigned>(io.stackLeft()), static_cast<unsigned>(worker.stackLeft(0)), static_cast<unsigned>(worker.stackLeft(1)));
    }
}

bool MapView::powerAllowsUpdates() {
    Device* supply = nullptr;
    if (device_get_first_active_by_type(&POWER_SUPPLY_TYPE, &supply) != ERROR_NONE) {
        // No battery to look at (powered by USB, or the board reports nothing): Wi-Fi alone decides
        return true;
    }
    bool allowed = true;
    PowerSupplyPropertyValue value {};
    if (power_supply_supports_property(supply, POWER_SUPPLY_PROP_IS_CHARGING) &&
        power_supply_get_property(supply, POWER_SUPPLY_PROP_IS_CHARGING, &value) == ERROR_NONE && value.int_value != 0) {
        allowed = true;
    } else if (power_supply_supports_property(supply, POWER_SUPPLY_PROP_CAPACITY) &&
        power_supply_get_property(supply, POWER_SUPPLY_PROP_CAPACITY, &value) == ERROR_NONE) {
        allowed = value.int_value >= UPDATE_MIN_BATTERY;
    }
    device_put(supply);
    return allowed;
}

void MapView::showToast(const char* text) {
    snprintf(toastText, sizeof(toastText), "%s", text);
    toastUntil = nowMs() + TOAST_SHOW_MS;
    dirty = true;
}

void MapView::startFollowing() {
    if (!position.hasFix()) {
        // Location is the user's to switch on, in the menu or in Tactility's settings
        following = false;
        showToast(position.unavailableMessage());
        return;
    }
    if (following && !headingUp) {
        // Second press: the direction of travel up, from the GPS course while moving
        headingUp = true;
        headingFilter.reset();
        showToast("Heading up");
    } else if (following) {
        // Third press: free again, the map keeps its turn until N or the compass
        following = false;
        headingUp = false;
        dirty = true;
        return;
    } else {
        following = true;
        headingUp = false;
        setBearing(0.0f, true);
    }
    centerOnPosition();
    dirty = true;
}

void MapView::toggleLocation() {
    const bool enable = !LocationSource::isEnabled();
    if (!LocationSource::setEnabled(enable)) {
        showToast("Could not change the location setting");
        return;
    }
    showToast(enable ? "Location on" : "Location off");
    readLocation();
}

void MapView::endPinch(float scale, int fromX, int fromY, int toX, int toY) {
    pinching = false;
    io.hold(false);
    // Snaps to the nearest whole zoom level, the point under the fingers at the start stays under them
    const int newZoom = snapZoom(zoom, scale, MAX_VIEW_ZOOM);
    const double factor = std::ldexp(1.0, newZoom - zoom);
    // pinchWorldX, pinchWorldY is the world point under (fromX, fromY) at the start; the bearing may have changed since
    const double worldX = pinchWorldX;
    const double worldY = pinchWorldY;
    (void)fromX;
    (void)fromY;
    double toWorldX;
    double toWorldY;
    screenToWorldOffset(toX - width / 2.0, toY - height / 2.0, toWorldX, toWorldY);
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), newZoom);
    centerX = std::fmod(worldX * factor - toWorldX + worldSize, worldSize);
    centerY = std::fmin(std::fmax(worldY * factor - toWorldY, 0.0), worldSize);
    pinchTurning = false;
    zoom = newZoom;
    dirty = true;
    updateWanted();
}

void MapView::readLocation() {
    lastLocationCheck = nowMs();
    const bool hadFix = position.hasFix();
    const PositionState before = position.state();
    PositionSource& source = replaying ? static_cast<PositionSource&>(replay) : static_cast<PositionSource&>(location);
    if (!position.update(source)) {
        return;
    }
    if (!hadFix && position.hasFix()) {
        logInfo("First position, accuracy %d m", static_cast<int>(position.fix().accuracy));
    }
    if (recorder.isActive() && position.hasFix()) {
        recorder.addFix(position.fix());
    }
    if (following && position.hasFix()) {
        // The turn first: the position's place on the screen depends on it while navigating
        if (headingUp && headingFilter.update(position.fix().course, position.fix().speed, nowMs())) {
            setBearing(headingFilter.heading(), false);
        }
        centerOnPosition();
    }
    if (before != position.state() && !position.hasFix()) {
        following = false;
        headingUp = false;
    }
    followRoute(nowMs());
    dirty = true;
}

void MapView::saveView() {
    SavedView view;
    view.lat = worldYToLat(centerY, zoom);
    view.lon = worldXToLon(centerX, zoom);
    view.zoom = zoom;
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/view.txt", rootPath);
    auto save = [&] { return writeSavedView(path, view); };
    if (!runOnCard(io, save)) {
        logError("View not saved to %s", path);
    }
}

void MapView::saveShot(const char* name) {
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/%s.bmp", rootPath, name);
    auto save = [&] { return saveBmp(path, frame, width, height); };
    logInfo("Screenshot %s: %s", path, runOnCard(io, save) ? "saved" : "failed");
}

void MapView::run() {
    while (!exitRequested) {
        const uint32_t now = nowMs();
        probe.heartbeat(now);
        probe.phase("keys");
        KeyEvent events[8];
        const int count = input.readKeys(events, 8);
        bool enterThisPoll = false;
        if (count > 0) {
            markInput(now);
        }
        for (int i = 0; i < count; i++) {
            if (events[i].key == keys::ENTER) {
                handleEnter(events[i].pressed, now);
                enterThisPoll = enterThisPoll || events[i].pressed;
            } else if (events[i].pressed) {
                handleKey(events[i].key);
            }
        }
        if (enterHeld && !enterThisPoll && now - enterSeenAt > ENTER_RELEASE_MS) {
            handleEnter(false, now);
        }
        if (enterHeld && !enterLong && now - enterDownAt >= LONG_PRESS_MS) {
            enterLong = true;
            longClick();
        }
        if (input.takeDevicesChanged()) {
            // The trackball was switched between keyboard and mouse mode
            controls.setInputs({ input.hasKeyboard(), input.hasTouch(), input.hasPointer() });
            cursorShown = false;
            pointerAcceleration.reset();
            dirty = true;
        }
        const uint32_t afterKeys = nowMs();
        probe.phase("pointer");
        handleMouse(now);
        handleTouch(now);
        const uint32_t afterInput = nowMs();
        if (!idle && now - lastInputAt >= IDLE_AFTER_MS) {
            idle = true;
            worker.setIdle(true);
            downloader.setIdle(true);
        }
        if (!updatesAllowed && now - lastInputAt >= UPDATE_IDLE_MS && now - lastPowerCheck >= POWER_CHECK_MS) {
            // Outdated regions update quietly once the map has been left alone, on Wi-Fi and with enough power
            lastPowerCheck = now;
            if (Downloader::isOnline() && powerAllowsUpdates()) {
                updatesAllowed = true;
                downloader.setUpdatesAllowed(true);
            }
        }
        if (now - lastLocationCheck >= LOCATION_CHECK_MS) {
            readLocation();
        }
        const uint32_t afterLocation = nowMs();
        probe.phase("downloader");
        pollDownloader(now);
        pollGpxImport();
        recorder.flushIfDue(io, now);
        if (recorder.state() == TrackRecorder::State::Recording && now - lastRecordingDraw >= 1000 && screen == Screen::Map) {
            // The elapsed time on the indicator
            lastRecordingDraw = now;
            dirty = true;
        }
        const uint32_t afterDownloader = nowMs();
        probe.phase("gaps");
        pollGaps(now);
        probe.phase("labels");
        pollLabelFonts();
        updateLabels(now);
        updateSmooth(now);
        if (browsedCounter.poll(now) && screen == Screen::Download) {
            dirty = true;
        }
        updateWork(now);
        updateNavigation(now);
        probe.phase("loop");
        const uint32_t afterGaps = nowMs();

        if (waitingRoute && navigator.state() != Navigator::State::Searching) {
            waitingRoute = false;
        }
        if (!waitingIdle && !waitingRoute) {
            const DebugScript::Step step = script.next(now);
            switch (step.action) {
                case DebugScript::Action::Key:
                    markInput(now);
                    if (step.key == keys::ENTER) {
                        handleEnter(true, now);
                        handleEnter(false, now);
                    } else if (step.key == 'c') {
                        // "key c" in a debug script is a trackball click in mouse mode
                        click(true);
                    } else {
                        handleKey(step.key);
                    }
                    break;
                case DebugScript::Action::Shot:
                    render();
                    saveShot(step.name);
                    break;
                case DebugScript::Action::Goto:
                    centerOn(step.lat, step.lon, step.zoom);
                    break;
                case DebugScript::Action::WaitIdle:
                    waitingIdle = true;
                    break;
                case DebugScript::Action::Exit:
                    exitRequested = true;
                    break;
                case DebugScript::Action::Touch:
                    syntheticTouch = step;
                    syntheticTouchStart = now;
                    syntheticTouchActive = true;
                    syntheticTouchEnded = false;
                    break;
                case DebugScript::Action::Squeeze:
                    squeezeMemory(static_cast<size_t>(step.zoom) * 1024);
                    break;
                case DebugScript::Action::RouteBench: {
                    char path[PATH_MAX_LENGTH];
                    snprintf(path, sizeof(path), "%s/%s", rootPath, step.name);
                    navigator.bench(path, step.zoom >= 3 ? RouteProfile::Count : static_cast<RouteProfile>(step.zoom));
                    break;
                }
                case DebugScript::Action::Navigate:
                    navigateTo(step.lat, step.lon, static_cast<RouteProfile>(std::min(step.zoom, 2)), false);
                    break;
                case DebugScript::Action::WaitRoute:
                    waitingRoute = true;
                    break;
                case DebugScript::Action::Replay:
                    startReplay(step.name);
                    break;
                case DebugScript::Action::TouchOnly:
                    input.setTouchOnly(true);
                    cursorShown = false;
                    controls.setInputs({ input.hasKeyboard(), input.hasTouch(), input.hasPointer() });
                    dirty = true;
                    break;
                default:
                    break;
            }
        } else if (viewTimed && !dirty) {
            waitingIdle = false;
        }

        if (subscribed) {
            AppEvent event;
            while (app_event_poll(&appEvents, &event) == ERROR_NONE) {
                if (event.type == APP_EVENT_CLOSE) {
                    exitRequested = true;
                }
            }
        }
        if (cursorShown && now - cursorMovedAt >= POINTER_HIDE_MS) {
            // A pointer that is not used does not cover the map
            cursorShown = false;
            controls.setHover(Control::None);
            dirty = true;
        }
        if (worker.takeChanged() && !isFullScreen()) {
            dirty = true;
        }
        if (attribution.update(now)) {
            dirty = true;
        }
        if (static_cast<int32_t>(now - toastUntil) >= 0 && toastUntil != 0) {
            toastUntil = 0;
            dirty = true;
        }
        const uint32_t beforeRender = nowMs();
        if (dirty && !exitRequested) {
            render();
        }
        // A pass that takes long is a freeze the user feels, the log says where the time went
        const uint32_t done = nowMs();
        if (done - now > SLOW_LOOP_MS) {
            logInfo("Slow loop %u ms: keys %u, mouse+touch %u, location %u, downloader %u, gaps %u, other %u, render %u", done - now,
                afterKeys - now, afterInput - afterKeys, afterLocation - afterInput, afterDownloader - afterLocation, afterGaps - afterDownloader,
                beforeRender - afterGaps, done - beforeRender);
        }
        // Sleeps until the next input poll, waking early for app events and new positions
        uint32_t flags = 0;
        if (animation.active || turn.active) {
            // Animation frames come as fast as they can be drawn, one tick lets the render lanes on this core go on
            vTaskDelay(1);
        } else if (!subscribed && location.bit() == 0) {
            // Without a claimed bit the wait would return at once
            vTaskDelay(pdMS_TO_TICKS(LOOP_INTERVAL_MS));
        } else if (task_event_group_wait_any(&eventGroup, &flags, pdMS_TO_TICKS(LOOP_INTERVAL_MS)) == ERROR_NONE &&
            (flags & location.bit()) != 0) {
            readLocation();
        }
    }
}

} // namespace maps
