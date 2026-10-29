// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"

#include "../platform/Platform.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace maps {

namespace {

constexpr int MENU_WIDTH = 220;
constexpr int ABOUT_LINES = 40;
constexpr int ABOUT_LINE_LENGTH = 64;

const char* const MENU_LABELS[] = { "Download area", "Regions", "Settings", "About and licences", "Exit" };

const char* const ABOUT_PARAGRAPHS[] = {
    "Tactility Maps " APP_VERSION,
    "Offline vector maps for Tactility.",
    "",
    "Map data \xC2\xA9 OpenStreetMap contributors, available under the Open Database Licence (ODbL): openstreetmap.org/copyright",
    "",
    "Vector tiles \xC2\xA9 OpenMapTiles (openmaptiles.org), served by OpenFreeMap (openfreemap.org).",
    "",
    "Root certificates of Google Trust Services and Let's Encrypt are included to reach OpenFreeMap over HTTPS.",
    "",
    "W/S or drag to scroll. Esc, Backspace or tap the title to go back.",
};

} // namespace

void MapView::setListHint(ListView& list, const char* keyHint, const char* touchHint) {
    // One line per input the board has, keys first
    char text[128];
    const bool touch = input.hasTouch();
    const bool keys = input.hasKeyboard() || !touch;
    snprintf(text, sizeof(text), "%s%s%s", keys ? keyHint : "", keys && touch ? "\n" : "", touch ? touchHint : "");
    list.setHint(text);
}

ListView* MapView::activeList() {
    switch (screen) {
        case Screen::Regions:
            return &regionList;
        case Screen::Settings:
            return &settingList;
        default:
            return nullptr;
    }
}

void MapView::closeScreen() {
    screen = Screen::Map;
    dialog.close();
    dirty = true;
    updateWanted();
}

// ---------------------------------------------------------------------------------------------------------------
// Menu

void MapView::openMenu() {
    menu.clear();
    for (const char* label : MENU_LABELS) {
        menu.add(label);
    }
    menu.layoutPopup(width, height, overlay.font(), std::min(MENU_WIDTH, width - 16));
    menuOpen = true;
    dirty = true;
}

void MapView::closeMenu() {
    menuOpen = false;
    dirty = true;
}

void MapView::chooseMenu(MenuItem item) {
    menuOpen = false;
    dirty = true;
    controls.hide();
    cursorShown = false;
    switch (item) {
        case MenuItem::Download:
            screen = Screen::Download;
            break;
        case MenuItem::Regions:
            openRegions();
            break;
        case MenuItem::Settings:
            openSettings();
            break;
        case MenuItem::About:
            openAbout();
            break;
        case MenuItem::Exit:
            // The only way out by trackball or touch, LVGL's swipe back does not run while the map has the screen
            exitRequested = true;
            break;
        default:
            break;
    }
}

void MapView::handleListAction(UiAction action) {
    dirty = true;
    if (menuOpen) {
        if (action == UiAction::Chosen) {
            chooseMenu(static_cast<MenuItem>(menu.selected()));
        } else if (action == UiAction::Back) {
            closeMenu();
        }
        return;
    }
    if (action == UiAction::Back) {
        closeScreen();
        return;
    }
    if (screen == Screen::Regions) {
        if (action == UiAction::Chosen) {
            chooseRegion();
        } else if (action == UiAction::Delete) {
            askDeleteRegion();
        }
    } else if (screen == Screen::Settings && action == UiAction::Chosen) {
        toggleSetting(static_cast<SettingItem>(settingList.selected()));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Regions

void MapView::openRegions() {
    regionCount = 0;
    auto read = [&] {
        for (int i = 0; i < store.regionCount() && regionCount < TileStore::MAX_REGIONS; i++) {
            char path[PATH_MAX_LENGTH];
            snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, store.regionName(i));
            RegionMeta& meta = regions[regionCount];
            if (readRegionMeta(path, meta)) {
                snprintf(meta.name, sizeof(meta.name), "%s", store.regionName(i));
                regionCount++;
            }
        }
        return true;
    };
    runOnCard(io, read);
    const int selected = screen == Screen::Regions ? regionList.selected() : 0;
    fillRegionList();
    regionList.select(selected);
    screen = Screen::Regions;
    dirty = true;
}

void MapView::fillRegionList() {
    regionList.clear();
    regionList.setTitle("< Regions");
    if (regionCount == 0) {
        regionList.setHint("No regions yet. Menu, Download area.");
    } else {
        setListHint(regionList, "Enter: show  Del: delete  Esc: back", "Tap: show  Hold: delete  Tap <: back");
    }
    const char* latest = downloader.latestVersion();
    const Downloader::Status status = downloader.status();
    for (int i = 0; i < regionCount; i++) {
        const RegionMeta& meta = regions[i];
        char label[ListView::TEXT_LENGTH];
        char detail[ListView::TEXT_LENGTH];
        const bool active = (status.state == Downloader::State::Downloading || status.state == Downloader::State::Connecting) &&
            strcmp(status.region, meta.name) == 0;
        if (active || !meta.isComplete()) {
            const int64_t done = active ? status.done : meta.done;
            snprintf(label, sizeof(label), "%s  %s %d%%", meta.name, active ? "downloading" : "paused",
                static_cast<int>(meta.total > 0 ? done * 100 / meta.total : 0));
        } else if (latest[0] != '\0' && meta.version[0] != '\0' && strcmp(latest, meta.version) != 0) {
            snprintf(label, sizeof(label), "%s  outdated", meta.name);
        } else {
            snprintf(label, sizeof(label), "%s  %s", meta.name, strcmp(meta.name, BROWSE_REGION) == 0 ? "viewed online" : "complete");
        }
        snprintf(detail, sizeof(detail), "%.1f MB %s", meta.bytes / 1e6, meta.date[0] != '\0' ? meta.date + 2 : "");
        regionList.add(label, detail);
    }
    regionList.layoutFullScreen(width, height, overlay.font());
}

void MapView::chooseRegion() {
    if (regionList.selected() >= regionCount) {
        return;
    }
    const RegionMeta& meta = regions[regionList.selected()];
    if (!meta.isComplete()) {
        downloader.download(meta.name);
    }
    showRegion(meta);
    screen = Screen::Map;
}

void MapView::askDeleteRegion() {
    if (regionList.selected() >= regionCount) {
        return;
    }
    char message[80];
    snprintf(message, sizeof(message), "Delete %s? Tiles other regions use are kept.", regions[regionList.selected()].name);
    dialog.open(message, "Delete", true);
    dialog.layout(width, height, overlay.font());
    dirty = true;
}

void MapView::deleteRegion() {
    if (screen != Screen::Regions || regionList.selected() >= regionCount) {
        return;
    }
    const char* name = regions[regionList.selected()].name;
    downloader.remove(name);
    store.removeRegion(name);
    worker.lock();
    worker.cache().clear();
    worker.unlock();
    // The list without it, the files go in the background
    for (int i = regionList.selected(); i + 1 < regionCount; i++) {
        regions[i] = regions[i + 1];
    }
    regionCount--;
    const int selected = regionList.selected();
    fillRegionList();
    regionList.select(selected);
    dirty = true;
}

void MapView::showRegion(const RegionMeta& meta) {
    // The highest zoom level at which the whole region fits on the screen
    int fitZoom = meta.maxZoom;
    while (fitZoom > 0) {
        const double regionWidth = lonToWorldX(meta.box.east, fitZoom) - lonToWorldX(meta.box.west, fitZoom);
        const double regionHeight = latToWorldY(meta.box.south, fitZoom) - latToWorldY(meta.box.north, fitZoom);
        if (regionWidth <= width && regionHeight <= height) {
            break;
        }
        fitZoom--;
    }
    following = false;
    centerOn((meta.box.south + meta.box.north) / 2.0, (meta.box.west + meta.box.east) / 2.0, fitZoom);
}

// ---------------------------------------------------------------------------------------------------------------
// Settings

void MapView::openSettings() {
    fillSettingList();
    settingList.select(0);
    screen = Screen::Settings;
    dirty = true;
}

void MapView::fillSettingList() {
    const int selected = settingList.selected();
    settingList.clear();
    settingList.setTitle("< Settings");
    setListHint(settingList, "Enter: change  Esc: back", "Tap: change  Tap <: back");
    settingList.add("SD card cache", settings.sdCache ? "On" : "Off");
    settingList.add("Prepared tiles", settings.deflatePrepared ? "Deflated" : "Raw");
    settingList.add("Smooth pinch", monochrome ? "Off (display)" : (settings.smoothPinch ? "On" : "Off"));
    settingList.add("Download while browsing", settings.browseDownload ? "On" : "Off");
    settingList.layoutFullScreen(width, height, overlay.font());
    settingList.select(selected);
}

void MapView::toggleSetting(SettingItem item) {
    switch (item) {
        case SettingItem::SdCache:
            settings.sdCache = !settings.sdCache;
            worker.setSdCache(settings.sdCache);
            break;
        case SettingItem::PreparedTiles:
            // Tiles converted from now on use the new kind, both kinds are read
            settings.deflatePrepared = !settings.deflatePrepared;
            worker.setDeflatePrepared(settings.deflatePrepared);
            break;
        case SettingItem::BrowseDownload:
            settings.browseDownload = !settings.browseDownload;
            break;
        case SettingItem::SmoothPinch:
            if (monochrome) {
                return;
            }
            settings.smoothPinch = !settings.smoothPinch;
            if (settings.smoothPinch && pinchBase == nullptr) {
                pinchBase = static_cast<uint16_t*>(largeAlloc(static_cast<size_t>(width) * height * sizeof(uint16_t)));
            } else if (!settings.smoothPinch && !pinching) {
                largeFree(pinchBase);
                pinchBase = nullptr;
            }
            break;
        default:
            return;
    }
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/settings.txt", rootPath);
    auto write = [&] { return writeSettings(path, settings); };
    if (!runOnCard(io, write)) {
        showToast("Could not save the settings");
    }
    fillSettingList();
    dirty = true;
}

// ---------------------------------------------------------------------------------------------------------------
// About and licences

void MapView::openAbout() {
    controls.hide();
    cursorShown = false;
    aboutScroll = 0;
    aboutLines = 0;
    if (aboutBuffer == nullptr) {
        aboutBuffer = static_cast<char*>(largeAlloc(ABOUT_LINES * ABOUT_LINE_LENGTH));
        if (aboutBuffer == nullptr) {
            return;
        }
    }
    screen = Screen::About;
    menuOpen = false;
    dirty = true;
}

void MapView::scrollAbout(int lines) {
    const int visible = (height - normalFont.lineHeight() - 8) / normalFont.lineHeight();
    aboutScroll = std::clamp(aboutScroll + lines, 0, std::max(0, aboutLines - visible));
    dirty = true;
}

void MapView::drawAbout() {
    Font& font = overlay.font();
    constexpr int padding = 8;
    if (aboutLines == 0) {
        // Wrapped once when the page opens
        for (const char* paragraph : ABOUT_PARAGRAPHS) {
            if (aboutLines >= ABOUT_LINES) {
                break;
            }
            if (paragraph[0] == '\0') {
                aboutBuffer[aboutLines++ * ABOUT_LINE_LENGTH] = '\0';
                continue;
            }
            aboutLines += wrapText(font, paragraph, width - 2 * padding, aboutBuffer + aboutLines * ABOUT_LINE_LENGTH, ABOUT_LINE_LENGTH,
                ABOUT_LINES - aboutLines);
        }
    }
    canvas.fillRect(0, 0, width, height, theme.panel);
    const int lineHeight = font.lineHeight();
    font.drawText(canvas, padding, 3, "< About and licences", theme.text);
    const int top = lineHeight + padding;
    canvas.fillRect(0, top - 3, width, 1, theme.border);
    const int visible = (height - top) / lineHeight;
    for (int i = 0; i < visible && aboutScroll + i < aboutLines; i++) {
        font.drawText(canvas, padding, top + i * lineHeight, aboutBuffer + (aboutScroll + i) * ABOUT_LINE_LENGTH, theme.text);
    }
    if (aboutLines > visible) {
        const int barHeight = std::max(8, (height - top) * visible / aboutLines);
        const int barTop = top + (height - top - barHeight) * aboutScroll / std::max(1, aboutLines - visible);
        canvas.fillRect(width - 3, barTop, 2, barHeight, theme.muted);
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Download area

BoundingBox MapView::frameBox() const {
    return screenBounds(downloadView.frame, centerX, centerY, zoom, width, height);
}

void MapView::drawDownloadFrame() {
    drawDownloadScreen(canvas, overlay.font(), theme, downloadView, planDownload(frameBox(), MAX_SOURCE_ZOOM));
}

void MapView::startDownload() {
    const DownloadPlan plan = planDownload(frameBox(), MAX_SOURCE_ZOOM);
    if (!plan.fits) {
        showToast("Too large for one download. Zoom in to cover less.");
        return;
    }
    RegionMeta meta {};
    auto makeFolder = [&] {
        for (int number = 1; number < 100; number++) {
            snprintf(meta.name, sizeof(meta.name), "area%02d", number);
            char folder[PATH_MAX_LENGTH];
            snprintf(folder, sizeof(folder), "%s/maps/%s", rootPath, meta.name);
            struct stat info {};
            if (stat(folder, &info) != 0) {
                mkdir(folder, 0777);
                return true;
            }
        }
        return false;
    };
    runOnCard(io, makeFolder);
    meta.box = plan.box;
    meta.minZoom = 0;
    meta.maxZoom = plan.maxZoom;
    meta.total = plan.tiles;
    meta.done = 0;
    meta.bytes = 0;
    time_t now = time(nullptr);
    struct tm local {};
    localtime_r(&now, &local);
    strftime(meta.date, sizeof(meta.date), "%Y-%m-%d", &local);
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, meta.name);
    auto write = [&] { return writeRegionMeta(path, meta); };
    if (!runOnCard(io, write)) {
        logError("Could not write %s", path);
        return;
    }
    store.addRegion(meta.name);
    store.setRegionBounds(meta.name, meta.box, meta.minZoom, meta.maxZoom);
    logInfo("New region %s: %d tiles up to z%d, about %d KB", meta.name, static_cast<int>(plan.tiles), plan.maxZoom,
        static_cast<int>(plan.estimatedBytes / 1024));
    downloader.download(meta.name);
    screen = Screen::Map;
    dirty = true;
    updateWanted();
}

void MapView::handleDownloadKey(uint32_t key) {
    switch (key) {
        case keys::ENTER:
        case keys::YES:
            startDownload();
            break;
        case keys::ESCAPE:
        case keys::BACKSPACE:
        case keys::NO:
            closeScreen();
            break;
        case ' ':
        case 'm':
        case 'M':
        case 'g':
        case 'G':
            // Only moving and zooming the map under the frame here
            break;
        default:
            handleMapKey(key);
            break;
    }
    dirty = true;
}

} // namespace maps
