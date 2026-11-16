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
constexpr int ABOUT_LINES = 56;
constexpr int ABOUT_LINE_LENGTH = 64;

/** The tiles of the main menu, in grid order (left to right, top to bottom). */
struct MenuEntry {
    MenuAction action;
    MenuIcon icon;
    const char* label;
};

const MenuEntry MENU_ENTRIES[] = {
    { MenuAction::Search, MenuIcon::Search, "Search" },
    { MenuAction::Download, MenuIcon::Download, "Download" },
    { MenuAction::Maps, MenuIcon::Maps, "Maps" },
    { MenuAction::Waypoints, MenuIcon::Waypoints, "Waypoints" },
    { MenuAction::Track, MenuIcon::Track, "Track" },
    { MenuAction::Gpx, MenuIcon::Gpx, "GPX" },
    { MenuAction::Location, MenuIcon::Location, "Location" },
    { MenuAction::Settings, MenuIcon::Settings, "Settings" },
    { MenuAction::About, MenuIcon::About, "About" },
};

const char* const DOWNLOAD_CHOICES[] = { "Choose area", "Choose region" };

/** A paragraph that starts with this character is a section heading. */
constexpr char ABOUT_HEADING = '#';
/** Marks a wrapped heading line in the line buffer, so drawAbout() can draw it in the heading style. */
constexpr char ABOUT_HEADING_LINE = '\x01';

/** A touch-only board gets other words for the scrolling hint, so this one is found again by its address. */
const char* const ABOUT_SCROLL_HINT = "W/S or drag to scroll. Esc, Backspace, < or a swipe to the right go back.";

const char* const ABOUT_PARAGRAPHS[] = {
    "#Tactility Maps " APP_VERSION,
    "Designed by Adam Szilv\xC3\xA1" "si",
    "Offline vector maps for Tactility.",
    "",
    "#Map data and licences",
    "\xC2\xA9 OpenMapTiles \xC2\xA9 OpenStreetMap contributors",
    "Map data \xC2\xA9 OpenStreetMap contributors, available under the Open Database Licence (ODbL): openstreetmap.org/copyright",
    "",
    "Vector tiles \xC2\xA9 OpenMapTiles (CC BY 4.0, openmaptiles.org), served by OpenFreeMap (openfreemap.org).",
    "",
    "Root certificates of Google Trust Services and Let's Encrypt are included to reach OpenFreeMap over HTTPS.",
    "",
    "Label font: Adwaita Sans (SIL Open Font License 1.1). Place icons: Maki by Mapbox (CC0).",
    "",
    "#Elevation",
    "Terrain (Download terrain in a region's actions; Settings, Terrain) comes from AWS Terrain Tiles (tilezen/joerd, open data on AWS).",
    "ArcticDEM (north of 60\xC2\xB0N) created from DigitalGlobe, Inc. imagery and funded under NSF awards 1043681, 1559691, and 1542736.",
    "Australia terrain data \xC2\xA9 Commonwealth of Australia (Geoscience Australia).",
    "Austria terrain data \xC2\xA9 offene Daten \xC3\x96sterreichs - Digitales Gel\xC3\xA4ndemodell (DGM) \xC3\x96sterreich.",
    "Canada terrain data contains information licensed under the Open Government Licence - Canada.",
    "Europe terrain data produced using Copernicus data and information funded by the European Union - EU-DEM layers.",
    "Mexico terrain data source: INEGI, Continental relief.",
    "New Zealand terrain data: Crown copyright Land Information New Zealand (LINZ).",
    "Norway terrain data \xC2\xA9 Kartverket.",
    "United Kingdom terrain data \xC2\xA9 Environment Agency copyright and/or database right 2015. All rights reserved.",
    "United States 3DEP (formerly NED) and global GMTED2010 and SRTM terrain data courtesy of the U.S. Geological Survey; global ETOPO1 ocean bathymetry courtesy of NOAA.",
    "Root certificates of Amazon and Starfield are included to reach the elevation tiles over HTTPS.",
    "",
    "#Marked trails",
    "Hiking and cycling routes (Settings, Marked trails; off unless you turn it on): OpenStreetMap data \xC2\xA9 OpenStreetMap contributors (ODbL), fetched once per region through the Overpass API (overpass-api.de).",
    "",
    "#Controls",
    ABOUT_SCROLL_HINT,
    "",
    "#This board",
    nullptr,  // the screen size and the input kinds, written when the page opens
};

/** What can be done with a region or a waypoint, the rows of their action popups. */
enum RegionAction : int {
    REGION_SHOW,
    REGION_UPDATE,
    REGION_CONTINUE,
    REGION_PAUSE,
    REGION_RENAME,
    REGION_DELETE,
    REGION_TERRAIN,
    REGION_TRAILS
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
            return &catalogBrowser.list();
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
    } else if (searchSheet.isOpen() && screen == Screen::Map) {
        // One level back in the search sheet (a category list to the main list), else closes it
        handleSearchEvent(searchSheet.handleKey(keys::ESCAPE));
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

void MapView::readMainMenuState(MainMenuState& state) {
    state = {};
    // While a download runs or is paused its details come first, so every input reaches Pause and Cancel
    const Downloader::Status status = downloader.status();
    const bool running = status.state == Downloader::State::Downloading || status.state == Downloader::State::Updating ||
        status.state == Downloader::State::Connecting;
    if (running || pausedRegion[0] != '\0') {
        const char* what = status.state == Downloader::State::Updating ? "Update" : "Download";
        if (running && status.total > 0) {
            snprintf(state.download, sizeof(state.download), "%s %d %%", what, static_cast<int>(status.done * 100 / status.total));
        } else {
            snprintf(state.download, sizeof(state.download), "%s %s", what, running ? "starting" : "paused");
        }
    }
    // A route: show it again, stop following it, or clear it (it is kept until then, also across restarts)
    if (navigator.hasRoute()) {
        state.route = navigating ? 2 : 1;
    }
    state.locationOn = LocationSource::isEnabled();
    if (recorder.isActive()) {
        state.track = recorder.state() == TrackRecorder::State::Paused ? 2 : 1;
    }
}

void MapView::buildMainMenu() {
    const uint16_t focus = menuGrid.focusAction();
    const bool shown = menuGrid.focusShown();
    readMainMenuState(menuShown);
    menuGrid.clear();
    menuGrid.setTitle("Menu");
    menuGrid.setGrid(3, 3);
    menuGrid.setCorner(static_cast<uint16_t>(MenuAction::Exit), "Exit");
    // Only a board with a typing keyboard has keys to tell about; touch and trackball need no explanation
    menuGrid.setHint(input.hasKeyboard() ? "WASD: move  Enter: open  Esc: close" : nullptr);
    for (const MenuEntry& entry : MENU_ENTRIES) {
        const char* detail = nullptr;
        if (entry.action == MenuAction::Location) {
            detail = menuShown.locationOn ? "On" : "Off";
        } else if (entry.action == MenuAction::Track && menuShown.track != 0) {
            detail = menuShown.track == 2 ? "Paused" : "Recording";
        }
        menuGrid.addTile(static_cast<uint16_t>(entry.action), entry.icon, entry.label, detail);
    }
    if (menuShown.download[0] != '\0') {
        menuGrid.addStatus(static_cast<uint16_t>(MenuAction::DownloadDetails), MenuIcon::Download, menuShown.download);
    }
    if (menuShown.route == 2) {
        menuGrid.addStatus(static_cast<uint16_t>(MenuAction::RouteStop), MenuIcon::Stop, "Stop");
    } else if (menuShown.route == 1) {
        menuGrid.addStatus(static_cast<uint16_t>(MenuAction::RouteShow), MenuIcon::Route, "Show route");
    }
    if (menuShown.route != 0) {
        menuGrid.addStatus(static_cast<uint16_t>(MenuAction::RouteClear), MenuIcon::Clear, "Clear route");
    }
    menuGrid.layout(width, height, overlay.font(), &overlay.smallFont());
    menuGrid.setFocusAction(focus, shown);
}

void MapView::refreshMainMenu() {
    MainMenuState state;
    readMainMenuState(state);
    if (!(state == menuShown)) {
        buildMainMenu();
        dirty = true;
    }
}

void MapView::openMenu() {
    buildMainMenu();
    // It opens where it was left, without a highlight until the first key or trackball step
    menuGrid.setFocusAction(lastMenuFocus, false);
    popup = Popup::Main;
    menuOpen = true;
    dirty = true;
}

void MapView::closeMenu() {
    if (mainMenuOpen()) {
        lastMenuFocus = menuGrid.focusAction();
    }
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

void MapView::handleMainMenuEvent(const GridEvent& event) {
    dirty = true;
    if (event.result == GridResult::Chosen) {
        chooseMenuAction(static_cast<MenuAction>(event.action));
    } else if (event.result == GridResult::Back) {
        closeMenu();
    }
}

void MapView::chooseMenuAction(MenuAction action) {
    dirty = true;
    if (action == MenuAction::Location) {
        // The only entry that keeps the menu open, its detail shows the new state
        toggleLocation();
        buildMainMenu();
        return;
    }
    // Back from a sub-menu or a screen opens the menu on the entry it was left from
    lastMenuFocus = static_cast<uint16_t>(action);
    menuOpen = false;
    switch (action) {
        case MenuAction::DownloadDetails:
            openDownloadCard();
            return;
        case MenuAction::RouteShow:
            fillRouteCard();
            fitRoute();
            return;
        case MenuAction::RouteStop:
            stopNavigation();
            showToast("Navigation stopped, the route stays");
            return;
        case MenuAction::RouteClear:
            clearRoute();
            return;
        default:
            break;
    }
    controls.hide();
    cursorShown = false;
    // Screens opened from here go back to the menu
    returnToMenu = true;
    switch (action) {
        case MenuAction::Download:
            openDownloadChoice();
            break;
        case MenuAction::Maps:
            openRegions();
            break;
        case MenuAction::Waypoints:
            openWaypoints();
            break;
        case MenuAction::Track:
            openTrackMenu();
            break;
        case MenuAction::Gpx:
            openGpxFiles();
            break;
        case MenuAction::Settings:
            openSettings();
            break;
        case MenuAction::About:
            openAbout();
            break;
        case MenuAction::Search:
            returnToMenu = false;
            openSearch();
            break;
        case MenuAction::Exit:
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
            // The main menu is the tile grid, it never comes here
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
                if (screen != Screen::Catalog) {
                    // It bailed with a toast, so there is no screen to come back from
                    returnToMenu = false;
                }
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
        case Popup::GpxActions:
            closeMenu();
            chooseGpxAction(chosen);
            break;
        case Popup::RouteStart:
            closeMenu();
            chooseRouteStart(chosen);
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
                if (waypointList.selected() == 0) {
                    addWaypointHere();
                } else {
                    openWaypointActions();
                }
            } else if (action == UiAction::Delete) {
                askDeleteWaypoint(listedWaypoint());
            }
            break;
        case Screen::GpxFiles:
            if (action == UiAction::Chosen) {
                openGpxActions();
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
        regionList.setHint("No maps yet. Menu, Download.");
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
    snprintf(actionRegion, sizeof(actionRegion), "%s", meta.name);
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
    if (meta.isComplete() && !active && strcmp(meta.name, BROWSE_REGION) != 0) {
        add("Download terrain", REGION_TERRAIN);
        if (settings.trails) {
            add("Download trails", REGION_TRAILS);
        }
    }
    if (canType()) {
        add("Rename", REGION_RENAME);
    }
    add("Delete", REGION_DELETE);
    layoutPopup();
}

void MapView::chooseRegionAction(int action) {
    // The list may have been rebuilt since the popup opened, so the region is found again by its folder name
    int found = -1;
    for (int i = 0; i < regionCount; i++) {
        if (strcmp(regions[i].name, actionRegion) == 0) {
            found = i;
            break;
        }
    }
    if (found < 0) {
        showToast("The list changed");
        dirty = true;
        return;
    }
    // The steps that follow (rename, delete, update) work on the selected row
    regionList.select(found);
    const RegionMeta& meta = regions[found];
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
        case REGION_TERRAIN: {
            // Elevation for shading and contour lines: its size is asked first (a height file is 128 KB on the card)
            const int64_t tiles = tileRange(meta.box, TERRAIN_ZOOM).count();
            char message[128];
            if (tiles > Downloader::MAX_TERRAIN_TILES) {
                showToast("Region too large for terrain");
                break;
            }
            snprintf(message, sizeof(message), "Terrain for %s: %d tiles, about %.1f MB download, %.1f MB on the card. Download?",
                regionTitle(meta), static_cast<int>(tiles), static_cast<double>(tiles) * 0.115, static_cast<double>(tiles) * 0.131);
            dialogPurpose = DialogPurpose::Terrain;
            dialog.open(message, "Download", false);
            dialog.layout(width, height, overlay.font());
            break;
        }
        case REGION_TRAILS:
            // One query to the Overpass API for the region's box (opt-in: Settings, Marked trails)
            downloader.downloadTrails(meta.name);
            showToast("Downloading trails");
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
    if (promptPurpose == PromptPurpose::CatalogSearch) {
        openCatalogResults(prompt.text());
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
    if (dialogPurpose == DialogPurpose::Terrain) {
        if (screen == Screen::Regions && regionList.selected() < regionCount) {
            const RegionMeta& meta = regions[regionList.selected()];
            downloader.downloadTerrain(meta.name);
            showToast("Downloading terrain");
        }
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
    // Filled again after each change: the list stays where it was scrolled to
    const int scrolled = settingList.scrollTop();
    fillSettingsList(settingList, settings, monochrome);
    setListHint(settingList, "Enter: change  Esc: back", "Tap: change  Swipe right: back");
    settingList.layoutFullScreen(width, height, overlay.font());
    settingList.restoreScroll(scrolled);
    settingList.select(selected);
}

void MapView::switchMapStyle() {
    if (monochrome) {
        showToast("Not on this display");
        dirty = true;
        return;
    }
    settings.outdoorStyle = !settings.outdoorStyle;
    palette = settings.outdoorStyle ? &outdoorPalette() : &colorPalette();
    theme = uiTheme(*palette, monochrome);
    worker.setPalette(palette);
    // Peaks show in the outdoor style only
    applyLabelSettings();
    char path[PATH_MAX_LENGTH];
    snprintf(path, sizeof(path), "%s/settings.txt", rootPath);
    auto write = [&] { return writeSettings(path, settings); };
    if (!runOnCard(io, write)) {
        showToast("Could not save the settings");
    }
    showToast(settings.outdoorStyle ? "Map style: Outdoor" : "Map style: Standard");
    dirty = true;
}

void MapView::toggleSetting(SettingItem item) {
    // Kept so a failed write can be undone: the screen and the card must not disagree
    const Settings before = settings;
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
        case SettingItem::RouteMode:
            // The mode a new route starts with; the card can switch it for one route
            settings.routeProfile = (settings.routeProfile + 1) % static_cast<int>(RouteProfile::Count);
            break;
        case SettingItem::WalkTrails:
            settings.walkTrails = !settings.walkTrails;
            navigator.setPreferTrails(settings.walkTrails);
            break;
        case SettingItem::Trails:
            settings.trails = !settings.trails;
            worker.setTrails(settings.trails);
            downloader.setTrailsWithDownloads(settings.trails);
            break;
        case SettingItem::Terrain:
            settings.terrain = !settings.terrain;
            worker.setTerrain(settings.terrain);
            downloader.setTerrainWithDownloads(settings.terrain);
            break;
        case SettingItem::PointerSpeed:
            settings.pointerSpeed = (settings.pointerSpeed + 1) % PointerAcceleration::SPEEDS;
            pointerAcceleration.setSpeed(settings.pointerSpeed);
            break;
        case SettingItem::SmoothPinch:
            if (monochrome) {
                showToast("Not on this display");
                dirty = true;
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
        // Back to what is on the card, and what the toggle applied in memory with it
        settings = before;
        worker.setSdCache(settings.sdCache);
        worker.setDeflatePrepared(settings.deflatePrepared);
        worker.setTerrain(settings.terrain);
        worker.setTrails(settings.trails);
        pointerAcceleration.setSpeed(settings.pointerSpeed);
        applyLabelSettings();
        showToast("Could not save the settings");
    }
    fillSettingList();
    dirty = true;
}

// ---------------------------------------------------------------------------------------------------------------
// About and licences

void MapView::openAbout() {
    // From the menu it goes back there, from the attribution on the map back to the map
    // The buffer comes first so that a failure changes nothing on screen
    if (aboutBuffer == nullptr) {
        aboutBuffer = static_cast<char*>(largeAlloc(ABOUT_LINES * ABOUT_LINE_LENGTH));
        if (aboutBuffer == nullptr) {
            showToast("Not enough memory");
            return;
        }
    }
    returnToMenu = menuOpen || returnToMenu;
    controls.hide();
    cursorShown = false;
    aboutScroll = 0;
    aboutLines = 0;
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
        char boardLine[ABOUT_LINE_LENGTH];
        snprintf(boardLine, sizeof(boardLine), "Screen %d x %d, %s", width, height, input.hasKeyboard() ? "with a keyboard" : "touch only");
        for (const char* paragraph : ABOUT_PARAGRAPHS) {
            if (paragraph == nullptr) {
                paragraph = boardLine;
            } else if (paragraph == ABOUT_SCROLL_HINT && !input.hasKeyboard()) {
                paragraph = "Drag to scroll. < or a swipe to the right goes back.";
            }
            if (aboutLines >= ABOUT_LINES) {
                break;
            }
            if (paragraph[0] == '\0') {
                aboutBuffer[aboutLines++ * ABOUT_LINE_LENGTH] = '\0';
                continue;
            }
            const bool heading = paragraph[0] == ABOUT_HEADING;
            const int first = aboutLines;
            aboutLines += wrapText(font, heading ? paragraph + 1 : paragraph, width - 2 * padding, aboutBuffer + aboutLines * ABOUT_LINE_LENGTH,
                ABOUT_LINE_LENGTH, ABOUT_LINES - aboutLines);
            if (heading) {
                // The marker goes in front of each line; a line that is too long loses its last character
                for (int i = first; i < aboutLines; i++) {
                    char* line = aboutBuffer + i * ABOUT_LINE_LENGTH;
                    memmove(line + 1, line, ABOUT_LINE_LENGTH - 2);
                    line[0] = ABOUT_HEADING_LINE;
                    line[ABOUT_LINE_LENGTH - 1] = '\0';
                }
            }
        }
    }
    canvas.fillRect(0, 0, width, height, theme.panel);
    const int lineHeight = font.lineHeight();
    const int top = lineHeight + padding + 4;
    aboutHeader = headerLayout(0, 0, width, top - 2);
    drawHeader(canvas, font, theme, aboutHeader, "About and licences", false);
    const int visible = (height - top) / lineHeight;
    for (int i = 0; i < visible && aboutScroll + i < aboutLines; i++) {
        const char* line = aboutBuffer + (aboutScroll + i) * ABOUT_LINE_LENGTH;
        if (line[0] == ABOUT_HEADING_LINE) {
            // A heading is the accent colour and drawn twice, one pixel apart, which also reads as bold on a mono display
            font.drawText(canvas, padding, top + i * lineHeight, line + 1, theme.accent);
            font.drawText(canvas, padding + 1, top + i * lineHeight, line + 1, theme.accent);
        } else {
            font.drawText(canvas, padding, top + i * lineHeight, line, theme.text);
        }
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

void MapView::presentCatalog() {
    ListView& list = catalogBrowser.list();
    setListHint(list, "Enter: open/choose  Esc: back", "Tap: open/choose  Swipe right: back");
    list.layoutFullScreen(width, height, overlay.font());
    list.setHighlighted(false);
    screen = Screen::Catalog;
    dirty = true;
}

void MapView::openCatalog(int parent) {
    if (!loadCatalog()) {
        showToast("The region list is missing, reinstall the app");
        return;
    }
    catalogBrowser.showLevel(catalog, parent);
    presentCatalog();
}

void MapView::chooseCatalog() {
    int region = -1;
    switch (catalogBrowser.choose(catalog, region)) {
        case CatalogBrowser::Choice::Search:
            promptPurpose = PromptPurpose::CatalogSearch;
            prompt.open("Region name", "");
            dirty = true;
            break;
        case CatalogBrowser::Choice::Opened:
            presentCatalog();
            break;
        case CatalogBrowser::Choice::Download:
            planCatalogRegion(region);
            break;
        case CatalogBrowser::Choice::None:
            break;
    }
}

void MapView::openCatalogResults(const char* query) {
    if (!catalogBrowser.showResults(catalog, query)) {
        showToast("No region with that name");
        dirty = true;
        return;
    }
    presentCatalog();
}

void MapView::catalogBack() {
    if (!catalogBrowser.back(catalog)) {
        // Back to the choice it came from
        returnToMenu = false;
        closeScreen();
        openDownloadChoice();
        return;
    }
    presentCatalog();
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
