// SPDX-License-Identifier: GPL-3.0-or-later
#include "MapView.h"

#include "../platform/Platform.h"

#include <app/paths.h>

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

const char* const MENU_LABELS[] = { "Download map", "Downloaded maps", "Waypoints", "Add waypoint", "Record track", "GPX files", "Location", "Settings",
    "About and licences", "Exit" };
const char* const DOWNLOAD_CHOICES[] = { "Choose area", "Choose region" };

const char* const ABOUT_PARAGRAPHS[] = {
    "Tactility Maps " APP_VERSION,
    "Designed by Adam Szilv\xC3\xA1" "si",
    "Offline vector maps for Tactility.",
    "",
    "Map data \xC2\xA9 OpenStreetMap contributors, available under the Open Database Licence (ODbL): openstreetmap.org/copyright",
    "",
    "Vector tiles \xC2\xA9 OpenMapTiles (openmaptiles.org), served by OpenFreeMap (openfreemap.org).",
    "",
    "Root certificates of Google Trust Services and Let's Encrypt are included to reach OpenFreeMap over HTTPS.",
    "",
    "Label font: Adwaita Sans (SIL Open Font License 1.1). Place icons: Maki by Mapbox (CC0).",
    "",
    "W/S or drag to scroll. Esc, Backspace, < or a swipe to the right go back.",
};

/** The menu row with the running download's details, before the menu items. */
constexpr int MENU_DOWNLOAD_DETAILS = 100;

/** What can be done with a region or a waypoint, the rows of their action popups. */
enum RegionAction : int {
    REGION_SHOW,
    REGION_UPDATE,
    REGION_CONTINUE,
    REGION_PAUSE,
    REGION_RENAME,
    REGION_DELETE
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
        case Screen::Catalog:
            return &catalogList;
        case Screen::Waypoints:
            return &waypointList;
        case Screen::GpxFiles:
            return &gpxList;
        case Screen::Tracks:
            return &trackList;
        default:
            return nullptr;
    }
}

void MapView::closeScreen() {
    const bool backToMenu = returnToMenu && isFullScreen();
    screen = Screen::Map;
    returnToMenu = false;
    dialog.close();
    dirty = true;
    updateWanted();
    if (backToMenu) {
        openMenu();
    }
}

void MapView::goBack() {
    if (menuOpen) {
        handlePopupAction(UiAction::Back);
    } else if (screen == Screen::About) {
        closeScreen();
    } else if (activeList() != nullptr) {
        handleListAction(UiAction::Back);
    }
    dirty = true;
}

// ---------------------------------------------------------------------------------------------------------------
// Menu

void MapView::beginPopup(Popup kind, const char* title) {
    menu.clear();
    menu.setHighlighted(false);
    menu.setTitle(title);
    popup = kind;
    popupActionCount = 0;
}

void MapView::layoutPopup() {
    menu.layoutPopup(width, height, overlay.font(), std::min(MENU_WIDTH, width - 16));
    menuOpen = true;
    dirty = true;
}

void MapView::openMenu() {
    beginPopup(Popup::Main, "Menu");
    // While a download runs or is paused its details come first, so every input reaches Pause and Cancel
    const Downloader::Status status = downloader.status();
    const bool running = status.state == Downloader::State::Downloading || status.state == Downloader::State::Updating ||
        status.state == Downloader::State::Connecting;
    if (running || pausedRegion[0] != '\0') {
        char detail[16];
        snprintf(detail, sizeof(detail), "%s", running ? "starting" : "paused");
        if (running && status.total > 0) {
            snprintf(detail, sizeof(detail), "%d%%", static_cast<int>(status.done * 100 / status.total));
        }
        menu.add(status.state == Downloader::State::Updating ? "Update" : "Download", detail);
        popupActions[popupActionCount++] = MENU_DOWNLOAD_DETAILS;
    }
    int item = 0;
    for (const char* label : MENU_LABELS) {
        popupActions[popupActionCount++] = item++;
        // The track entry says what it does now
        if (strcmp(label, "Record track") == 0 && recorder.isActive()) {
            menu.add(recorder.state() == TrackRecorder::State::Paused ? "Track (paused)" : "Track (recording)");
        } else if (strcmp(label, "Location") == 0) {
            menu.add(label, LocationSource::isEnabled() ? "On" : "Off");
        } else {
            menu.add(label);
        }
    }
    layoutPopup();
}

void MapView::closeMenu() {
    menuOpen = false;
    dirty = true;
}

void MapView::openDownloadChoice() {
    // The same popup, now with the two ways to pick an area
    beginPopup(Popup::DownloadChoice, "Download map");
    for (const char* label : DOWNLOAD_CHOICES) {
        menu.add(label);
    }
    layoutPopup();
}

void MapView::chooseMenu(MenuItem item) {
    menuOpen = false;
    dirty = true;
    controls.hide();
    cursorShown = false;
    // Screens opened from here go back to the menu
    returnToMenu = true;
    switch (item) {
        case MenuItem::Download:
            openDownloadChoice();
            break;
        case MenuItem::Regions:
            openRegions();
            break;
        case MenuItem::Waypoints:
            openWaypoints();
            break;
        case MenuItem::AddWaypoint:
            openNewWaypoint(worldYToLat(centerY, zoom), worldXToLon(centerX, zoom));
            break;
        case MenuItem::RecordTrack:
            openTrackMenu();
            break;
        case MenuItem::GpxFiles:
            openGpxFiles();
            break;
        case MenuItem::Location:
            toggleLocation();
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
    if (!isFullScreen()) {
        returnToMenu = false;
    }
}

void MapView::handlePopupAction(UiAction action) {
    dirty = true;
    const Popup kind = popup;
    if (action == UiAction::Back) {
        // Sub-menus go back to the menu, the actions of a row back to their list, the menu closes
        closeMenu();
        if (kind == Popup::DownloadChoice || kind == Popup::Track) {
            openMenu();
        }
        return;
    }
    if (action != UiAction::Chosen) {
        return;
    }
    const int row = menu.selected();
    const int chosen = row >= 0 && row < popupActionCount ? popupActions[row] : row;
    switch (kind) {
        case Popup::Main:
            if (chosen == MENU_DOWNLOAD_DETAILS) {
                closeMenu();
                openDownloadCard();
            } else {
                chooseMenu(static_cast<MenuItem>(chosen));
            }
            break;
        case Popup::DownloadChoice:
            closeMenu();
            returnToMenu = true;
            if (row == 0) {
                returnToMenu = false;
                screen = Screen::Download;
                // The download frame works north up
                following = false;
                headingUp = false;
                setBearing(0.0f, false);
            } else {
                openCatalog(-1);
            }
            break;
        case Popup::Track:
            closeMenu();
            chooseTrackMenu(chosen);
            break;
        case Popup::RegionActions:
            closeMenu();
            chooseRegionAction(chosen);
            break;
        case Popup::WaypointActions:
            closeMenu();
            chooseWaypointAction(chosen);
            break;
    }
}

void MapView::handleListAction(UiAction action) {
    dirty = true;
    if (menuOpen) {
        handlePopupAction(action);
        return;
    }
    if (screen == Screen::Catalog) {
        if (action == UiAction::Chosen) {
            chooseCatalog();
        } else if (action == UiAction::Back) {
            catalogBack();
        }
        return;
    }
    if (action == UiAction::Back) {
        closeScreen();
        return;
    }
    switch (screen) {
        case Screen::Regions:
            if (action == UiAction::Chosen) {
                openRegionActions();
            } else if (action == UiAction::Delete) {
                askDeleteRegion();
            }
            break;
        case Screen::Waypoints:
            if (action == UiAction::Chosen) {
                openWaypointActions();
            } else if (action == UiAction::Delete) {
                askDeleteWaypoint(listedWaypoint());
            }
            break;
        case Screen::GpxFiles:
            if (action == UiAction::Chosen) {
                toggleGpxFile();
            }
            break;
        case Screen::Settings:
            if (action == UiAction::Chosen) {
                toggleSetting(static_cast<SettingItem>(settingList.selected()));
            }
            break;
        default:
            break;
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
    if (screen != Screen::Regions) {
        regionList.setHighlighted(false);
    }
    screen = Screen::Regions;
    dirty = true;
}

void MapView::fillRegionList() {
    regionList.clear();
    regionList.setTitle("Downloaded maps");
    if (regionCount == 0) {
        regionList.setHint("No maps yet. Menu, Download map.");
    } else {
        setListHint(regionList, "Enter: actions  R: rename  X: delete", "Tap: actions  Swipe right: back");
    }
    const Downloader::Status status = downloader.status();
    for (int i = 0; i < regionCount; i++) {
        const RegionMeta& meta = regions[i];
        char label[ListView::TEXT_LENGTH];
        char detail[ListView::TEXT_LENGTH];
        const bool active = (status.state == Downloader::State::Downloading || status.state == Downloader::State::Connecting) &&
            strcmp(status.region, meta.name) == 0;
        if (active || !meta.isComplete()) {
            const int64_t done = active ? status.done : meta.done;
            snprintf(label, sizeof(label), "%s  %s %d%%", regionTitle(meta), active ? "downloading" : "paused",
                static_cast<int>(meta.total > 0 ? done * 100 / meta.total : 0));
        } else if (status.state == Downloader::State::Updating && strcmp(status.region, meta.name) == 0) {
            snprintf(label, sizeof(label), "%s  updating %d%%", regionTitle(meta), static_cast<int>(status.total > 0 ? status.done * 100 / status.total : 0));
        } else if (isOutdated(meta)) {
            snprintf(label, sizeof(label), "%s  outdated", regionTitle(meta));
        } else {
            snprintf(label, sizeof(label), "%s  %s", regionTitle(meta), strcmp(meta.name, BROWSE_REGION) == 0 ? "viewed online" : "complete");
        }
        snprintf(detail, sizeof(detail), "%.1f MB %s", meta.bytes / 1e6, meta.date[0] != '\0' ? meta.date + 2 : "");
        regionList.add(label, detail);
    }
    regionList.layoutFullScreen(width, height, overlay.font());
}

void MapView::openRegionActions() {
    if (regionList.selected() >= regionCount) {
        return;
    }
    const RegionMeta& meta = regions[regionList.selected()];
    const Downloader::Status status = downloader.status();
    const bool active = downloader.isBusy() && strcmp(status.region, meta.name) == 0;
    beginPopup(Popup::RegionActions, regionTitle(meta));
    auto add = [&](const char* label, int action) {
        menu.add(label);
        popupActions[popupActionCount++] = action;
    };
    add("Show on map", REGION_SHOW);
    if (isOutdated(meta)) {
        add("Update now", REGION_UPDATE);
    }
    if (active && status.state != Downloader::State::Deleting) {
        add("Pause download", REGION_PAUSE);
    } else if (!meta.isComplete()) {
        add("Continue download", REGION_CONTINUE);
    }
    if (canType()) {
        add("Rename", REGION_RENAME);
    }
    add("Delete", REGION_DELETE);
    layoutPopup();
}

void MapView::chooseRegionAction(int action) {
    if (regionList.selected() >= regionCount) {
        return;
    }
    const RegionMeta& meta = regions[regionList.selected()];
    switch (action) {
        case REGION_SHOW:
            returnToMenu = false;
            showRegion(meta);
            screen = Screen::Map;
            break;
        case REGION_UPDATE: {
            // Updating costs a download of the whole region again, so it is asked first
            char message[96];
            snprintf(message, sizeof(message), "%s has newer map data (about %.0f MB). Update now?", regionTitle(meta), meta.bytes / 1e6);
            dialogPurpose = DialogPurpose::Update;
            dialog.open(message, "Update now", false);
            dialog.layout(width, height, overlay.font());
            break;
        }
        case REGION_CONTINUE:
            downloader.download(meta.name);
            showToast("Download continues");
            break;
        case REGION_PAUSE:
            downloader.cancel();
            showToast("Download paused");
            break;
        case REGION_RENAME:
            askRenameRegion();
            break;
        case REGION_DELETE:
            askDeleteRegion();
            break;
        default:
            break;
    }
    dirty = true;
}

bool MapView::isOutdated(const RegionMeta& meta) const {
    const char* latest = downloader.latestVersion();
    return meta.isComplete() && latest[0] != '\0' && meta.version[0] != '\0' && strcmp(latest, meta.version) != 0 &&
        strcmp(meta.name, BROWSE_REGION) != 0;
}

void MapView::askRenameRegion() {
    if (regionList.selected() >= regionCount) {
        return;
    }
    if (!canType()) {
        return;
    }
    const RegionMeta& meta = regions[regionList.selected()];
    const Downloader::Status status = downloader.status();
    if (downloader.isBusy() && strcmp(status.region, meta.name) == 0) {
        // Its meta.json is being written by the download
        showToast("Rename it when its download is finished");
        return;
    }
    promptPurpose = PromptPurpose::RenameRegion;
    prompt.open("Name of this region", regionTitle(meta));
    prompt.layout(width, height, overlay.font());
    dirty = true;
}

void MapView::confirmPrompt() {
    if (promptPurpose == PromptPurpose::RenameWaypoint) {
        renameWaypoint(prompt.text());
        return;
    }
    if (screen != Screen::Regions || regionList.selected() >= regionCount) {
        return;
    }
    RegionMeta& meta = regions[regionList.selected()];
    setRegionTitle(meta, prompt.text());
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, meta.name);
    // The folder keeps its short name, only the title changes
    RegionMeta stored {};
    auto write = [&] {
        if (!readRegionMeta(path, stored)) {
            return false;
        }
        setRegionTitle(stored, prompt.text());
        return writeRegionMeta(path, stored);
    };
    if (!runOnCard(io, write)) {
        showToast("Could not save the name");
    }
    const int selected = regionList.selected();
    fillRegionList();
    regionList.select(selected);
}

void MapView::confirmDialog() {
    if (dialogPurpose == DialogPurpose::CatalogDownload) {
        startCatalogDownload();
        return;
    }
    if (dialogPurpose == DialogPurpose::DeleteWaypoint) {
        deleteWaypoint();
        return;
    }
    if (dialogPurpose == DialogPurpose::CancelDownload) {
        // The downloader stops the job and removes the region, keeping tiles other regions use
        downloader.remove(cancelRegion);
        store.removeRegion(cancelRegion);
        pausedRegion[0] = '\0';
        showToast("Download cancelled");
        return;
    }
    if (dialogPurpose == DialogPurpose::Update) {
        if (screen == Screen::Regions && regionList.selected() < regionCount) {
            const RegionMeta& meta = regions[regionList.selected()];
            downloader.update(meta.name);
            returnToMenu = false;
            showRegion(meta);
            screen = Screen::Map;
        }
        return;
    }
    deleteRegion();
}

void MapView::askDeleteRegion() {
    if (regionList.selected() >= regionCount) {
        return;
    }
    char message[80];
    snprintf(message, sizeof(message), "Delete %s? Tiles other regions use are kept.", regionTitle(regions[regionList.selected()]));
    dialogPurpose = DialogPurpose::Delete;
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
    settingList.setHighlighted(false);
    screen = Screen::Settings;
    dirty = true;
}

void MapView::fillSettingList() {
    const int selected = settingList.selected();
    settingList.clear();
    settingList.setTitle("Settings");
    setListHint(settingList, "Enter: change  Esc: back", "Tap: change  Swipe right: back");
    settingList.add("SD card cache", settings.sdCache ? "On" : "Off");
    settingList.add("Prepared tiles", settings.deflatePrepared ? "Deflated" : "Raw");
    settingList.add("Smooth zoom", monochrome ? "Off (display)" : (settings.smoothPinch ? "On" : "Off"));
    settingList.add("Download while browsing", settings.browseDownload ? "On" : "Off");
    static const char* const POINTER_SPEEDS[] = { "Slow", "Normal", "Fast" };
    settingList.add("Pointer speed", POINTER_SPEEDS[std::clamp(settings.pointerSpeed, 0, 2)]);
    static const char* const LABEL_DENSITIES[] = { "Off", "Few", "Normal" };
    settingList.add("Labels", LABEL_DENSITIES[std::clamp(settings.labels, 0, 2)]);
    settingList.add("Latin names", settings.latinNames ? "On" : "Off");
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
        case SettingItem::Labels:
            settings.labels = (settings.labels + 1) % 3;
            applyLabelSettings();
            break;
        case SettingItem::LatinNames:
            settings.latinNames = !settings.latinNames;
            applyLabelSettings();
            break;
        case SettingItem::PointerSpeed:
            settings.pointerSpeed = (settings.pointerSpeed + 1) % PointerAcceleration::SPEEDS;
            pointerAcceleration.setSpeed(settings.pointerSpeed);
            break;
        case SettingItem::SmoothPinch:
            if (monochrome) {
                return;
            }
            settings.smoothPinch = !settings.smoothPinch;
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
    // From the menu it goes back there, from the attribution on the map back to the map
    returnToMenu = menuOpen || returnToMenu;
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
            // The last paragraph names keys: a board with only a touch screen gets the touch words
            if (paragraph == ABOUT_PARAGRAPHS[sizeof(ABOUT_PARAGRAPHS) / sizeof(ABOUT_PARAGRAPHS[0]) - 1] && !input.hasKeyboard()) {
                paragraph = "Drag to scroll. < or a swipe to the right goes back.";
            }
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
    const int top = lineHeight + padding + 4;
    aboutHeader = headerLayout(0, 0, width, top - 2);
    drawHeader(canvas, font, theme, aboutHeader, "About and licences", false);
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
    const DownloadPlan plan = planDownload(frameBox(), MAX_SOURCE_ZOOM);
    // How many of its tiles are browsed already, counted in the background once the frame stays put
    browsedCounter.request(plan, nowMs());
    int64_t onCard = -1;
    browsedCounter.count(plan, onCard);
    drawDownloadScreen(canvas, overlay.font(), theme, downloadView, plan, onCard);
}

bool MapView::makeRegionFolder(const char* title, RegionMeta& meta) {
    auto makeFolder = [&] {
        for (int number = 0; number < 100; number++) {
            regionFolderName(title, number, meta.name, sizeof(meta.name));
            // "browsed" belongs to browsing, a region called Browsed gets browsed2
            if (strcmp(meta.name, BROWSE_REGION) == 0) {
                continue;
            }
            char folder[PATH_MAX_LENGTH];
            snprintf(folder, sizeof(folder), "%s/maps/%s", rootPath, meta.name);
            struct stat info {};
            if (stat(folder, &info) != 0) {
                return mkdir(folder, 0777) == 0;
            }
        }
        return false;
    };
    return runOnCard(io, makeFolder);
}

void MapView::startDownload() {
    const DownloadPlan plan = planDownload(frameBox(), MAX_SOURCE_ZOOM);
    if (!plan.fits) {
        showToast("Too large for one download. Zoom in to cover less.");
        return;
    }
    // An area a region already covers is not downloaded a second time: an unfinished region continues instead
    int existing = -1;
    RegionMeta found {};
    auto findCovering = [&] {
        constexpr double SLACK = 1e-6;
        for (int i = 0; i < store.regionCount() && existing < 0; i++) {
            if (strcmp(store.regionName(i), BROWSE_REGION) == 0) {
                continue;
            }
            char path[PATH_MAX_LENGTH];
            snprintf(path, sizeof(path), "%s/maps/%s/meta.json", rootPath, store.regionName(i));
            if (readRegionMeta(path, found) && found.outline[0] == '\0' && found.minZoom <= 0 && found.maxZoom >= plan.maxZoom &&
                found.box.west <= plan.box.west + SLACK && found.box.south <= plan.box.south + SLACK &&
                found.box.east >= plan.box.east - SLACK && found.box.north >= plan.box.north - SLACK) {
                existing = i;
            }
        }
        return true;
    };
    runOnCard(io, findCovering);
    if (existing >= 0) {
        char text[96];
        if (found.isComplete()) {
            snprintf(text, sizeof(text), "Already downloaded: %s", regionTitle(found));
        } else {
            snprintf(text, sizeof(text), "Continuing the download of %s", regionTitle(found));
            pausedRegion[0] = '\0';
            downloader.download(store.regionName(existing));
        }
        logInfo("Frame covered by region %s", store.regionName(existing));
        showToast(text);
        screen = Screen::Map;
        dirty = true;
        updateWanted();
        return;
    }
    RegionMeta meta {};
    // Named after the smallest catalog region around the frame's centre for now; once its tiles are on the card the
    // downloader looks for the nearest town in them
    char title[sizeof(meta.title)] = "";
    const double centerLat = (plan.box.south + plan.box.north) / 2.0;
    const double centerLon = (plan.box.west + plan.box.east) / 2.0;
    if (!catalogTitle(centerLat, centerLon, title, sizeof(title))) {
        title[0] = '\0';
    }
    if (!makeRegionFolder(title, meta)) {
        showToast("Could not make a folder on the SD card");
        return;
    }
    setRegionTitle(meta, title);
    meta.box = plan.box;
    meta.minZoom = 0;
    meta.maxZoom = plan.maxZoom;
    meta.total = plan.tiles;
    logInfo("New region %s (%s): %d tiles up to z%d, about %d KB", meta.name, regionTitle(meta), static_cast<int>(plan.tiles), plan.maxZoom,
        static_cast<int>(plan.estimatedBytes / 1024));
    if (beginRegionDownload(meta)) {
        screen = Screen::Map;
        dirty = true;
        updateWanted();
    }
}

bool MapView::beginRegionDownload(RegionMeta& meta) {
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
        return false;
    }
    store.addRegion(meta.name);
    store.setRegionBounds(meta.name, meta.box, meta.minZoom, meta.maxZoom);
    downloader.download(meta.name);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Region catalog

bool MapView::loadCatalog() {
    if (catalog.isOpen()) {
        return true;
    }
    if (catalogPath[0] == '\0') {
        app_paths_get_assets_path(APP_ID, "regions.bin", catalogPath, sizeof(catalogPath));
        downloader.setCatalogPath(catalogPath);
    }
    // About 350 KB in PSRAM while the app runs, read once
    auto read = [&] { return catalogFile.load(catalogPath); };
    if (!runOnCard(io, read, IoPriority::Background) || !catalog.open(catalogFile.bytes(), catalogFile.size())) {
        logError("Region catalog %s not readable", catalogPath);
        return false;
    }
    logInfo("Region catalog: %d regions, %u bytes", catalog.count(), static_cast<unsigned>(catalogFile.size()));
    return true;
}

bool MapView::catalogTitle(double lat, double lon, char* out, size_t size) {
    if (!loadCatalog()) {
        return false;
    }
    const int index = catalog.smallestContaining(lat, lon);
    if (index < 0) {
        return false;
    }
    catalog.copyName(index, out, size);
    return true;
}

void MapView::openCatalog(int parent) {
    if (!loadCatalog()) {
        showToast("The region list is missing, reinstall the app");
        return;
    }
    catalogParent = parent;
    catalogList.clear();
    char name[ListView::TEXT_LENGTH];
    int rows = 0;
    if (parent >= 0) {
        catalog.copyName(parent, name, sizeof(name));
        catalogList.setTitle(name);
        char label[ListView::TEXT_LENGTH];
        snprintf(label, sizeof(label), "All of %s", name);
        catalogList.add(label);
        catalogRows[rows++] = parent;
    } else {
        catalogList.setTitle("Choose region");
    }
    int children[ListView::MAX_ITEMS];
    const int count = catalog.children(parent, children, ListView::MAX_ITEMS - rows);
    for (int i = 0; i < count; i++) {
        catalog.copyName(children[i], name, sizeof(name));
        catalogList.add(name, catalog.region(children[i]).childCount > 0 ? ">" : "");
        catalogRows[rows++] = children[i];
    }
    setListHint(catalogList, "Enter: open/choose  Esc: back", "Tap: open/choose  Swipe right: back");
    catalogList.layoutFullScreen(width, height, overlay.font());
    catalogList.select(0);
    catalogList.setHighlighted(false);
    screen = Screen::Catalog;
    dirty = true;
}

void MapView::chooseCatalog() {
    const int row = catalogList.selected();
    if (row < 0 || row >= catalogList.count()) {
        return;
    }
    const int region = catalogRows[row];
    // A region with parts opens its list; "All of ..." and regions without parts are downloaded
    if (region != catalogParent && catalog.region(region).childCount > 0) {
        openCatalog(region);
        return;
    }
    planCatalogRegion(region);
}

void MapView::catalogBack() {
    if (catalogParent < 0) {
        // Back to the choice it came from
        returnToMenu = false;
        closeScreen();
        openDownloadChoice();
        return;
    }
    const int child = catalogParent;
    openCatalog(catalog.region(catalogParent).parent);
    // Back on the row it came from
    for (int row = 0; row < catalogList.count(); row++) {
        if (catalogRows[row] == child) {
            catalogList.select(row);
        }
    }
}

void MapView::planCatalogRegion(int index) {
    // Zoom by zoom from the lowest, as long as the whole region still fits OpenFreeMap's limits
    CatalogPlan plan { index, -1, 0, 0 };
    for (int z = 0; z <= MAX_SOURCE_ZOOM; z++) {
        const int64_t room = MAX_DOWNLOAD_TILES - plan.tiles;
        const int64_t tiles = catalog.forEachTile(index, z, nullptr, nullptr, room);
        const uint64_t bytes = static_cast<uint64_t>(tiles) * typicalTileBytes(z);
        if (tiles > room || plan.bytes + bytes > MAX_DOWNLOAD_BYTES) {
            break;
        }
        plan.maxZoom = z;
        plan.tiles += tiles;
        plan.bytes += bytes;
    }
    char name[48];
    catalog.copyName(index, name, sizeof(name));
    if (plan.maxZoom < 0 || plan.tiles == 0) {
        showToast("This region is too large for one download");
        return;
    }
    catalogPlan = plan;
    char message[96];
    snprintf(message, sizeof(message), "%s: %d tiles, about %d MB, detail up to zoom %d. Download?", name, static_cast<int>(plan.tiles),
        static_cast<int>((plan.bytes + 500000) / 1000000), plan.maxZoom);
    dialogPurpose = DialogPurpose::CatalogDownload;
    dialog.open(message, "Download", false);
    dialog.layout(width, height, overlay.font());
    dirty = true;
}

void MapView::startCatalogDownload() {
    const CatalogPlan& plan = catalogPlan;
    const CatalogRegion region = catalog.region(plan.region);
    RegionMeta meta {};
    char name[sizeof(meta.outline)];
    catalog.copyName(plan.region, name, sizeof(name));
    if (!makeRegionFolder(name, meta)) {
        showToast("Could not make a folder on the SD card");
        return;
    }
    setRegionTitle(meta, name);
    snprintf(meta.outline, sizeof(meta.outline), "%s", name);
    meta.box = region.box;
    meta.minZoom = 0;
    meta.maxZoom = plan.maxZoom;
    meta.total = plan.tiles;
    logInfo("New region %s (%s, catalog outline): %d tiles up to z%d, about %d KB", meta.name, name, static_cast<int>(plan.tiles), plan.maxZoom,
        static_cast<int>(plan.bytes / 1024));
    if (beginRegionDownload(meta)) {
        returnToMenu = false;
        showRegion(meta);
        screen = Screen::Map;
        dirty = true;
    }
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
