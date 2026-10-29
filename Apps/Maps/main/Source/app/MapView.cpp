// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"

#include "../platform/Platform.h"
#include "../render/Compose.h"

#include <tactility/drivers/display.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cmath>
#include <cstdio>
#include <cstring>

namespace maps {

namespace {

/**
 * 3.5 MB of rendered tiles in PSRAM. With 32 (4 MB) a download, a conversion and panning together left as little
 * as 60 KB of PSRAM free, and downloaded tiles could not be kept.
 */
constexpr int TILE_CACHE_SLOTS = 28;
constexpr uint32_t LOOP_INTERVAL_MS = 15;
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

bool MapView::begin(Device* display, const char* root, const char* dataPath) {
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
    if (frame == nullptr || preview == nullptr) {
        logError("Out of memory");
        return false;
    }
    canvas = { frame, width, height };
    // From here on only the I/O task touches the SD card
    if (!io.start()) {
        logError("I/O task not started");
        return false;
    }
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
        if (normalFont.open(folder, NORMAL_FONTS, 3)) {
            normalFont.preload();
        } else {
            logError("No font found in %s, the map shows no text", folder);
        }
        if (smallFont.open(folder, SMALL_FONTS, 1)) {
            smallFont.preload();
        }
        script.open(root);
        return true;
    };
    runOnCard(io, loadState);
    logInfo("Map root %s, %d regions", root, regionsFound);
    overlay.setFonts(&normalFont, smallFont.isOpen() && smallFont.lineHeight() >= MIN_LINE_HEIGHT ? &smallFont : &normalFont);
    // Live pinch scaling is off on monochrome displays, e-paper could not show it
    if (settings.smoothPinch && !monochrome) {
        pinchBase = static_cast<uint16_t*>(largeAlloc(static_cast<size_t>(width) * height * sizeof(uint16_t)));
    }
    worker.setDeflatePrepared(settings.deflatePrepared);
    worker.setSdCache(settings.sdCache);
    if (!worker.start(&store, &io, palette, TILE_CACHE_SLOTS, root)) {
        return false;
    }
    input.begin();
    // The overlay adapts to the inputs the board has: key hints with a keyboard, focus without touch or pointer
    controls.setInputs({ input.hasKeyboard(), input.hasTouch(), input.hasPointer() });
    controls.layout(width, height, overlay.scaleBarTop(canvas));
    downloadView = downloadLayout(width, height, overlay.font().lineHeight());
    downloader.setTiming(settings.devProbe);
    if (!downloader.start(root, &store, &io)) {
        logError("Downloader not started");
    }
    if (settings.devProbe) {
        probe.watch(xTaskGetCurrentTaskHandle(), "main");
        probe.watch(worker.laneTask(0), "lane0");
        probe.watch(worker.laneTask(1), "lane1");
        probe.watch(io.taskHandle(), "io");
        probe.watch(downloader.controlTaskHandle(), "download");
        probe.watch(downloader.browseTaskHandle(), "browse");
        if (!probe.start()) {
            logError("Stall probes not started");
        }
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
    downloader.stop();
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
    normalFont.close();
    smallFont.close();
    output.end();
    largeFree(frame);
    frame = nullptr;
    largeFree(preview);
    preview = nullptr;
    largeFree(pinchBase);
    pinchBase = nullptr;
    largeFree(aboutBuffer);
    aboutBuffer = nullptr;
}

void MapView::centerOn(double lat, double lon, int newZoom) {
    zoom = newZoom;
    centerX = lonToWorldX(lon, zoom);
    centerY = latToWorldY(lat, zoom);
    dirty = true;
    updateWanted();
}

void MapView::setZoom(int newZoom, int anchorX, int anchorY) {
    if (newZoom < 0 || newZoom > MAX_VIEW_ZOOM || newZoom == zoom) {
        return;
    }
    // The world point under the anchor stays where it is on the screen
    const double offsetX = anchorX - width / 2.0;
    const double offsetY = anchorY - height / 2.0;
    const double scale = std::ldexp(1.0, newZoom - zoom);
    centerX = (centerX + offsetX) * scale - offsetX;
    centerY = (centerY + offsetY) * scale - offsetY;
    zoom = newZoom;
    dirty = true;
    updateWanted();
}

void MapView::pan(double dx, double dy) {
    if (dx == 0.0 && dy == 0.0) {
        return;
    }
    following = false;
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), zoom);
    centerX = std::fmod(centerX + dx + worldSize, worldSize);
    centerY = std::fmin(std::fmax(centerY + dy, 0.0), worldSize);
    dirty = true;
    updateWanted();
}

void MapView::updateWanted() {
    // The visible tiles first, nearest to the centre first, then a ring around them
    const double left = centerX - width / 2.0;
    const double top = centerY - height / 2.0;
    const int firstX = static_cast<int>(std::floor(left / TILE_SIZE)) - 1;
    const int firstY = static_cast<int>(std::floor(top / TILE_SIZE)) - 1;
    const int lastX = static_cast<int>(std::floor((left + width - 1) / TILE_SIZE)) + 1;
    const int lastY = static_cast<int>(std::floor((top + height - 1) / TILE_SIZE)) + 1;
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
    viewChangedAt = nowMs();
    viewTimed = false;
}

const uint16_t* MapView::lookupTile(const TileKey& tile, void* context) {
    return static_cast<MapView*>(context)->findTile(tile);
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
    return nullptr;
}

bool MapView::isViewComplete() {
    const double left = centerX - width / 2.0;
    const double top = centerY - height / 2.0;
    const int tileCount = 1 << zoom;
    worker.lock();
    bool complete = true;
    for (int y = static_cast<int>(std::floor(top / TILE_SIZE)); y <= static_cast<int>(std::floor((top + height - 1) / TILE_SIZE)) && complete; y++) {
        for (int x = static_cast<int>(std::floor(left / TILE_SIZE)); x <= static_cast<int>(std::floor((left + width - 1) / TILE_SIZE)) && complete; x++) {
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
    x = static_cast<int>(std::floor(width / 2.0 + dx + 0.5));
    y = static_cast<int>(std::floor(height / 2.0 + latToWorldY(position.fix().latitude, zoom) - centerY + 0.5));
    // Far outside the screen only the accuracy circle can still show
    const int reach = 4 * (width + height);
    return x > -reach && y > -reach && x < width + reach && y < height + reach;
}

void MapView::drawStatus() {
    char text[80];
    const Downloader::Status& status = lastStatus;
    if (status.state == Downloader::State::Downloading && status.total > 0) {
        snprintf(text, sizeof(text), "Downloading %s %d%%", status.region, static_cast<int>(status.done * 100 / status.total));
    } else if (downloader.isBrowsing()) {
        snprintf(text, sizeof(text), "Downloading map");
    } else if (status.state == Downloader::State::Connecting || status.state == Downloader::State::Deleting) {
        snprintf(text, sizeof(text), "%s", status.message);
    } else if (static_cast<int32_t>(nowMs() - messageUntil) < 0) {
        snprintf(text, sizeof(text), "%s", status.message);
    } else if (store.regionCount() == 0) {
        snprintf(text, sizeof(text), "No maps yet: Menu, Download area");
    } else {
        return;
    }
    overlay.drawPill(canvas, text, *palette);
}

void MapView::pollDownloader(uint32_t now) {
    if (downloader.takeTilesChanged()) {
        worker.tilesArrived();
        dirty = true;
    }
    if (!resumeChecked) {
        // A download that was interrupted continues when the map opens again
        resumeChecked = true;
        int unfinished = -1;
        RegionMeta meta {};
        auto find = [&] {
            for (int i = 0; i < store.regionCount() && unfinished < 0; i++) {
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
    if (now - lastStatusPoll < STATUS_POLL_MS) {
        return;
    }
    lastStatusPoll = now;
    const Downloader::Status status = downloader.status();
    if (status.state != lastStatus.state || status.done != lastStatus.done || strcmp(status.message, lastStatus.message) != 0) {
        if (status.state != lastStatus.state && (status.state == Downloader::State::Done || status.state == Downloader::State::Failed)) {
            messageUntil = now + MESSAGE_SHOW_MS;
        }
        lastStatus = status;
        dirty = true;
        // The Regions screen shows progress as it happens
        if (screen == Screen::Regions && status.state == Downloader::State::Done) {
            openRegions();
        }
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
    return following ? PositionButton::Following : PositionButton::Free;
}

void MapView::drawMap() {
    const uint32_t start = nowMs();
    worker.lock();
    const uint32_t locked = nowMs();
    composeView(frame, width, height, centerX, centerY, zoom, lookupTile, this, palette->placeholder);
    worker.unlock();
    frameLockMs = locked - start;
    frameComposeMs = nowMs() - locked;
    int markerX;
    int markerY;
    if (markerScreenPosition(markerX, markerY)) {
        if (position.state() == PositionState::Approximate) {
            overlay.drawApproximateMarker(canvas, markerX, markerY, position.fix().accuracy, *palette);
        } else {
            const float radius = position.accuracyRadius(metresPerPixel(position.fix().latitude, zoom));
            overlay.drawAccuracyCircle(canvas, markerX, markerY, radius, *palette);
            overlay.drawMarker(canvas, markerX, markerY, position.showsArrow(), position.fix().course, *palette);
        }
    }
}

void MapView::render() {
    const uint32_t start = nowMs();
    if (pinching) {
        // The picture under the fingers scaled live, sharp tiles follow when the fingers lift
        scaleView(pinchBase, frame, width, height, pinchScale, static_cast<float>(pinchStartX), static_cast<float>(pinchStartY),
            static_cast<float>(pinchX), static_cast<float>(pinchY), palette->placeholder);
        if (screen == Screen::Download) {
            // The frame and its bar stay put while the map scales under them
            overlay.setBottomInset(downloadView.bar.height);
            drawDownloadFrame();
        } else {
            overlay.setBottomInset(0);
        }
        overlay.drawAttributionMark(canvas, *palette);
        output.present(frame, 0, height);
        dirty = false;
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
            controls.draw(canvas, overlay.font(), theme, positionButton());
        }
        if (screen == Screen::Map) {
            drawStatus();
        }
    }
    if (menuOpen) {
        menu.draw(canvas, overlay.font(), theme);
    }
    if (dialog.isOpen()) {
        dialog.draw(canvas, overlay.font(), theme);
    }
    if (static_cast<int32_t>(nowMs() - toastUntil) < 0) {
        overlay.drawToast(canvas, toastText, theme);
    }
    if (cursorShown && screen == Screen::Map && !menuOpen) {
        // An arrow pointer, dark with a light rim so it shows on any map
        const auto x = static_cast<float>(cursorX);
        const auto y = static_cast<float>(cursorY);
        canvas.line(x, y, x, y + 13.0f, 4.0f, theme.panel);
        canvas.line(x, y, x + 9.0f, y + 9.0f, 4.0f, theme.panel);
        canvas.line(x, y, x, y + 13.0f, 2.0f, theme.text);
        canvas.line(x, y, x + 9.0f, y + 9.0f, 2.0f, theme.text);
    }
    const uint32_t presentStart = nowMs();
    output.present(frame, 0, height);
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

void MapView::showToast(const char* text) {
    snprintf(toastText, sizeof(toastText), "%s", text);
    toastUntil = nowMs() + TOAST_SHOW_MS;
    dirty = true;
}

void MapView::startFollowing() {
    if (!position.hasFix()) {
        // Location settings are the user's, the app only says where to find them
        following = false;
        showToast(position.unavailableMessage());
        return;
    }
    following = true;
    centerOn(position.fix().latitude, position.fix().longitude, zoom);
    dirty = true;
}

void MapView::endPinch(float scale, int fromX, int fromY, int toX, int toY) {
    pinching = false;
    // Snaps to the nearest whole zoom level, the point under the fingers at the start stays under them
    const int newZoom = snapZoom(zoom, scale, MAX_VIEW_ZOOM);
    const double factor = std::ldexp(1.0, newZoom - zoom);
    const double worldX = centerX + (fromX - width / 2.0);
    const double worldY = centerY + (fromY - height / 2.0);
    const double worldSize = std::ldexp(static_cast<double>(TILE_SIZE), newZoom);
    centerX = std::fmod(worldX * factor - (toX - width / 2.0) + worldSize, worldSize);
    centerY = std::fmin(std::fmax(worldY * factor - (toY - height / 2.0), 0.0), worldSize);
    zoom = newZoom;
    dirty = true;
    updateWanted();
}

void MapView::readLocation() {
    lastLocationCheck = nowMs();
    const bool hadFix = position.hasFix();
    const PositionState before = position.state();
    if (!position.update(location)) {
        return;
    }
    if (!hadFix && position.hasFix()) {
        logInfo("First position, accuracy %d m", static_cast<int>(position.fix().accuracy));
    }
    if (following && position.hasFix()) {
        centerOn(position.fix().latitude, position.fix().longitude, zoom);
    }
    if (before != position.state() && !position.hasFix()) {
        following = false;
    }
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
        const uint32_t afterKeys = nowMs();
        handleMouse(now);
        handleTouch(now);
        const uint32_t afterInput = nowMs();
        if (!idle && now - lastInputAt >= IDLE_AFTER_MS) {
            idle = true;
            worker.setIdle(true);
            downloader.setIdle(true);
        }
        if (now - lastLocationCheck >= LOCATION_CHECK_MS) {
            readLocation();
        }
        const uint32_t afterLocation = nowMs();
        pollDownloader(now);
        const uint32_t afterDownloader = nowMs();
        pollGaps(now);
        const uint32_t afterGaps = nowMs();

        if (!waitingIdle) {
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
        if (!subscribed && location.bit() == 0) {
            // Without a claimed bit the wait would return at once
            vTaskDelay(pdMS_TO_TICKS(LOOP_INTERVAL_MS));
        } else if (task_event_group_wait_any(&eventGroup, &flags, pdMS_TO_TICKS(LOOP_INTERVAL_MS)) == ERROR_NONE &&
            (flags & location.bit()) != 0) {
            readLocation();
        }
    }
}

} // namespace maps
