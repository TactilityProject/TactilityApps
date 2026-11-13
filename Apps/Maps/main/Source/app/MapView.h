// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "BinFontText.h"
#include "Debug.h"
#include "DisplayOutput.h"
#include "Downloader.h"
#include "GpxImport.h"
#include "Input.h"
#include "IoTask.h"
#include "BrowsedCounter.h"
#include "../ui/TouchKeyboard.h"
#include "../ui/BackgroundWork.h"
#include "LabelFontMaker.h"
#include "LabelStore.h"
#include "Navigator.h"
#include "../route/RouteFollower.h"
#include "../ui/NavigationBar.h"
#include "../ui/RouteCard.h"
#include "LocationSource.h"
#include "StallProbe.h"
#include "TileWorker.h"
#include "TrackRecorder.h"

#include "../render/LabelLayout.h"
#include "../tiles/DownloadPlan.h"
#include "../tiles/RegionCatalog.h"
#include "../tiles/RegionMeta.h"
#include "../tiles/TileStore.h"
#include "../ui/Canvas.h"
#include "../ui/Controls.h"
#include "../ui/HeadingFilter.h"
#include "../ui/DownloadProgress.h"
#include "../ui/DownloadFrame.h"
#include "../ui/Gestures.h"
#include "../ui/MapOverlay.h"
#include "../ui/PointerAcceleration.h"
#include "../ui/WaypointSheet.h"
#include "../ui/SearchSheet.h"
#include "SearchIndexJob.h"
#include "SearchTask.h"
#include "../search/SearchEngine.h"
#include "../ui/PositionTracker.h"
#include "../ui/Settings.h"
#include "../ui/GpxLayer.h"
#include "../ui/Waypoints.h"
#include "../ui/Widgets.h"

#include <app/event.h>
#include <tactility/device.h>

#include <cstdint>

namespace maps {

struct AffineScratch;

#define APP_VERSION "0.5.0"
constexpr const char* APP_ID = "tactility.maps";

/**
 * The whole app while it runs fullscreen with LVGL stopped: the map, the menu, the Regions, Settings and About
 * screens, and the download frame, all drawn by the app itself.
 */
/** Positions played from a GPX track, one point a second, for testing navigation indoors ("replay" debug command). */
class ReplaySource final : public PositionSource {
public:
    static constexpr int MAX_POINTS = 2000;
    static constexpr uint32_t STEP_MS = 1000;
    bool load(const char* text, size_t size);
    /** The accuracy reported (0: 5 m) and correlated noise of noiseM metres added to the points (0.4.1). */
    void setNoise(float accuracyM, float noiseM, uint32_t seed);
    void clear() { count = 0; }
    bool isActive() const { return count > 0; }
    int size() const { return count; }
    /** The track point played last, for the log. */
    int pointIndex() const { return lastIndex; }
    PositionRead read(PositionFix& fix) override;

private:
    double points[MAX_POINTS][2] {};
    /** Per point: noise north and east in metres. */
    float noise[MAX_POINTS][2] {};
    float accuracy = 5.0f;
    int count = 0;
    int lastIndex = -1;
    int next = 0;
    uint32_t lastStep = 0;
};

class MapView {
public:
    bool begin(Device* display, const char* root, const char* dataPath);
    void run();
    void end();

private:
    enum class Screen : uint8_t {
        Map,
        Download,
        Regions,
        Settings,
        About,
        Catalog,
        Waypoints,
        GpxFiles,
        Tracks
    };

    enum class MenuItem : uint8_t {
        Download,
        Regions,
        Waypoints,
        AddWaypoint,
        RecordTrack,
        GpxFiles,
        Location,
        Settings,
        About,
        Exit,
        Search,
        Count
    };

    enum class Popup : uint8_t {
        Main,
        DownloadChoice,
        Track,
        RegionActions,
        WaypointActions,
        /** Where a route starts when there is no GPS fix (M5c). */
        RouteStart
    };

    enum class SettingItem : uint8_t {
        SdCache,
        PreparedTiles,
        SmoothPinch,
        BrowseDownload,
        PointerSpeed,
        Labels,
        LatinNames,
        RouteMode,
        Count
    };

    // MapView.cpp: the map itself
    void centerOn(double lat, double lon, int zoom);
    /**
     * "Navigate here": a route to lat, lon with the profile. With a GPS fix it starts there; without one the app asks
     * where it starts (on the map, the screen centre, or an approximate position), unless ask is false (debug scripts:
     * the screen centre).
     */
    void navigateTo(double lat, double lon, RouteProfile profile, bool ask = true);
    /** Polls the route search: a toast when it ends, the preview when a route was found. */
    void updateNavigation(uint32_t now);
    void fillRouteCard();
    /** Another mode on the card: the same start and goal again; a running search is cancelled first. */
    void chooseRouteMode(RouteProfile next);
    void searchRouteAgain();
    /** Starts the search from a start point; fromPosition: the start follows the position when the profile changes. */
    void requestRoute(double fromLat, double fromLon, bool fromPosition, const char* toast);
    void openRouteStartChoice();
    void chooseRouteStart(int choice);
    /** Picking the start on the map: a tap there, or Enter for the crosshair; Esc cancels. */
    void beginPickStart();
    void endPickStart(bool chosen, double lat, double lon);
    void drawPickStart();
    void fitRoute();
    void handleRouteCardButton(RouteCard::Button button);
    void startNavigation();
    /** The controls laid out for the screen, the menu button below the navigation bar while navigating. */
    void layoutControls();
    /** Centres the map on the position; while navigating the position sits low on the screen, to see ahead. */
    void centerOnPosition();
    void stopNavigation();
    void clearRoute();
    /** A new position while following: the next instruction, off route, arrival. */
    void followRoute(uint32_t now);
    /** The route line, above the roads and below labels and pins. */
    void drawRoute();
    void startReplay(const char* name, float accuracyM = 0.0f, float noiseM = 0.0f, uint32_t seed = 0);
    /** Remembers the centre and zoom, so the map opens there next time. */
    void saveView();
    void setZoom(int zoom, int anchorX, int anchorY);
    /**
     * Zooms by whole levels around a screen point: animated over about 220 ms with the cached tiles scaled, then
     * sharp tiles. Straight to the new level on monochrome and e-paper displays, or with smooth zoom off.
     */
    void zoomBy(int steps, int anchorX, int anchorY);
    bool smoothZoom() const { return settings.smoothPinch && !monochrome; }
    /** Draws the map through the affine compositor while a zoom animation or a pinch runs. */
    void drawScaledMap();
    static const uint16_t* lookupCached(const TileKey& tile, void* context);
    /** Moves the map by a screen offset: on a turned map the keys and fingers move it in screen directions. */
    void pan(double dx, double dy);
    void updateWanted();
    /** Turns the map so that the direction bearing (degrees clockwise from north) is up; animated when wanted. */
    void setBearing(float degrees, bool animate);
    /** Sets the bearing at once; want false leaves the tile list alone (live turning with two fingers). */
    void applyBearing(float degrees, bool want = true);
    static constexpr float TURN_ANIMATION_MS = 300.0f;
    /** Q and E turn the map by this much. */
    static constexpr float KEY_TURN_DEGREES = 15.0f;
    /** How far two fingers must twist before a turned map turns with them. */
    static constexpr float PINCH_TURN_START = 15.0f;
    /** The same for a map that is north up, so a pinch rarely turns it by accident. */
    static constexpr float PINCH_TURN_START_NORTH = 45.0f;
    bool isRotated() const { return bearing != 0.0f; }
    /** A screen offset from the screen centre as a world offset at the current zoom, and back. */
    void screenToWorldOffset(double sx, double sy, double& wx, double& wy) const;
    void worldToScreenOffset(double wx, double wy, double& sx, double& sy) const;
    /** The world pixels at zoom the screen covers, turned or not. */
    void viewBounds(double& left, double& top, double& right, double& bottom) const;
    /** The compass that shows north on a turned map, top right; tapping it turns the map back. */
    Rect compassRect() const;
    void drawCompass();
    void render();
    void drawMap();
    /** Places the labels of the tiles on screen for the current view, when it settled or new label data came. */
    void placeLabels();
    void updateLabels(uint32_t now);
    /** Uses the label fonts the maker finished: opens their cache files and places the labels again. */
    void pollLabelFonts();
    /** Gives the labels the fonts of the app where they opened, else the system's. */
    void applyLabelFonts();
    void applyLabelSettings();
    void drawStatus();
    /** Opens the details of the running download, from a tap on the progress pill or Menu, Download. */
    void openDownloadCard();
    void fillDownloadCard();
    void handleCardButton(DownloadCard::Button button);
    void pollDownloader(uint32_t now);
    /** Hands the tiles that lack detail to the downloader, or says why they stay coarse. */
    void pollGaps(uint32_t now);
    Rect attributionMark() const;
    void readLocation();
    /** Charging, or the battery above the threshold, or no battery reported. */
    static bool powerAllowsUpdates();
    void showToast(const char* text);
    /** The position button and G: not following, following north up, following with the direction of travel up. */
    void startFollowing();
    /** Menu, Location: switches Tactility's location service on or off, the one setting the app changes. */
    void toggleLocation();
    bool markerScreenPosition(int& x, int& y) const;
    void saveShot(const char* name);
    bool isViewComplete();
    static const uint16_t* lookupTile(const TileKey& tile, void* context);
    const uint16_t* findTile(const TileKey& tile);
    void endPinch(float scale, int fromX, int fromY, int toX, int toY);
    bool isFullScreen() const {
        return screen == Screen::Regions || screen == Screen::Settings || screen == Screen::About || screen == Screen::Catalog ||
            screen == Screen::Waypoints || screen == Screen::GpxFiles || screen == Screen::Tracks;
    }

    // MapInput.cpp: keys, trackball and touch
    void handleKey(uint32_t key);
    void handleMapKey(uint32_t key);
    void handleEnter(bool pressed, uint32_t now);
    void handleMouse(uint32_t now);
    void handleTouch(uint32_t now);
    void handleGesture(const Gesture& gesture);
    void handleTap(int x, int y);
    void handleLongPress(int x, int y);
    void markInput(uint32_t now);
    /** A trackball or Enter click: the same as Enter on lists, the map's own action on the map. */
    void click(bool fromPointer);
    void longClick();
    /** A click of the mouse pointer: what a tap at the pointer does, on every screen. */
    void pointerClick();
    /**
     * Moves the pointer, which stops at the screen edges. Returns in overflowX, overflowY how far the movement went
     * past an edge: pushing against an edge moves the map or scrolls a list.
     */
    void moveCursor(int dx, int dy, uint32_t now, int& overflowX, int& overflowY);
    void pressControl(Control control);
    /** A tap or pointer click on the map: buttons and the attribution first. Returns true when something took it. */
    bool pressAt(int x, int y);
    PositionButton positionButton() const;

    // MapScreens.cpp: menu, Regions, Settings, About, download frame
    void openMenu();
    void closeMenu();
    void chooseMenu(MenuItem item);
    void handleListAction(UiAction action);
    /** What Enter, a click, a tap or Back did in the popup that is open. */
    void handlePopupAction(UiAction action);
    /** Opens a popup of kind with a title; rows are added to menu by the caller before layoutPopup(). */
    void beginPopup(Popup kind, const char* title);
    void layoutPopup();
    /** Back one level from a list, the popup or About: Swipe right, < or Esc. */
    void goBack();
    void openRegions();
    void fillRegionList();
    /** Enter, a tap or a long press on a region: what can be done with it. */
    void openRegionActions();
    void chooseRegionAction(int action);
    void askDeleteRegion();
    void deleteRegion();
    bool isOutdated(const RegionMeta& meta) const;
    /** The dialog's main button: delete or update the selected region. */
    void confirmDialog();
    /** Rename the selected region, with a keyboard. */
    void askRenameRegion();
    /** The text prompt's OK. */
    void confirmPrompt();
    void openSettings();
    void fillSettingList();
    void toggleSetting(SettingItem item);
    void openAbout();
    void scrollAbout(int lines);
    void drawAbout();
    void closeScreen();
    BoundingBox frameBox() const;
    void drawDownloadFrame();
    void startDownload();
    /** Creates maps/<folder>/ for a new region named after title; the folder name is 8.3-safe and not taken yet. */
    bool makeRegionFolder(const char* title, RegionMeta& meta);
    /** Writes a new region's meta.json, registers it and starts its download. */
    bool beginRegionDownload(RegionMeta& meta);
    // Region catalog: Menu, Download area, Choose region
    bool loadCatalog();
    void openDownloadChoice();
    void openCatalog(int parent);
    void chooseCatalog();
    void catalogBack();
    /** Works out the highest max zoom of a catalog region within the download limits, and asks to download it. */
    void planCatalogRegion(int index);
    void startCatalogDownload();
    /** The smallest catalog region around a point, as a title for a new download; false without the catalog. */
    bool catalogTitle(double lat, double lon, char* out, size_t size);

    // MapPlaces.cpp: waypoints, track recording, GPX files
    void loadWaypoints();
    void saveWaypoints();
    /** Opens the sheet for a new waypoint at a point: name, coordinates, distance, Save and Cancel. */
    void openNewWaypoint(double lat, double lon, const char* name = nullptr);
    /** How far the placed labels moved with the map since they were placed; false when they are not for this view. */
    bool labelOffset(int& x, int& y) const;
    /** The POI or house number under a screen point, -1 when none. */
    int placeAt(int x, int y) const;
    /** Opens the sheet of a placed POI or house number: name, type, coordinates, distance, Save as waypoint, Close. */
    void openPlaceSheet(int index);
    /**
     * Enter on the map while the controls show: the POI or house number nearest the screen centre (the crosshair),
     * then each next nearest. False when none is near the centre.
     */
    bool selectNearestPlace();
    void selectNextPlace();
    /** The same at my position, P and holding the position button; says why when there is none. */
    void openNewWaypointAtPosition();
    /** Opens the sheet of an existing waypoint: Rename, Delete, Close. */
    void openWaypointSheet(int index);
    // Search (M6, MapSearch.cpp)
    class CardSearchFile;
    bool loadSearchIndexes();
    void releaseSearch();
    SearchOrigin searchOrigin(bool& fromPosition) const;
    void openSearch();
    void closeSearch();
    void layoutSearch();
    void showRecentRows();
    void fillSearchRows(int from, int count, bool more);
    void runSearch();
    void moreSearchResults();
    void searchNearby(int category);
    void openSearchResult(const char* name, const char* type, int32_t x, int32_t y, const int32_t* box, const char* note);
    void chooseSearchRow(int index, bool recent);
    void handleSearchEvent(SearchSheet::Event event);
    void loadRecentSearches();
    void saveRecentSearches();
    /** logInfo, and while a debug script runs also a line in scriptlog.txt (measurements without a serial log). */
    void scriptLog(const char* format, ...);
    void handleSheetButton(WaypointSheet::Button button);
    /** Saves the new waypoint of the sheet and says so. */
    void saveNewWaypoint();
    void openWaypoints();
    void fillWaypointList();
    void goToWaypoint();
    /** Enter, a tap or a long press on a waypoint in the list: Go to, Rename, Delete. */
    void openWaypointActions();
    void chooseWaypointAction(int action);
    void askDeleteWaypoint(int index);
    void deleteWaypoint();
    void askRenameWaypoint(int index);
    void renameWaypoint(const char* name);
    /** The waypoint highlighted in the list, -1 when none. */
    int listedWaypoint() const;
    /** The waypoint pin under a screen point, -1 when none. */
    int waypointAt(int x, int y) const;
    void drawPlaces();
    void drawRecording();
    void openTrackMenu();
    void chooseTrackMenu(int row);
    void openTracks();
    void openGpxFiles();
    void toggleGpxFile();
    void pollGpxImport();
    void saveShownGpx();
    void loadShownGpx();
    /** "1.2 km NE" from my position (or the map centre) to a point. */
    void describeDistance(double lat, double lon, char* out, size_t size) const;
    bool worldToScreen(double lat, double lon, float& x, float& y) const;
    void handleDownloadKey(uint32_t key);
    void showRegion(const RegionMeta& meta);
    ListView* activeList();
    /** Sets a list's hint for the inputs this board has. */
    void setListHint(ListView& list, const char* keyHint, const char* touchHint);

    DisplayOutput output;
    IoTask io;
    Input input;
    BinFontText normalFont;
    BinFontText smallFont;
    BinFontText largeFont;
    /** Label fonts made from the app's assets, with Latin Extended-A (the system fonts stop at Latin-1). */
    BinFontText labelFonts[3];
    /** The waypoint and place sheet shows names: in the app's label font once it is made (Tactility's UI font stops at Latin-1). */
    Font& sheetFont() { return labelFonts[1].isOpen() ? static_cast<Font&>(labelFonts[1]) : overlay.font(); }
    LabelFontMaker fontMaker;
    /** Browsed tiles inside the download frame, for its estimate. */
    BrowsedCounter browsedCounter;
    /**
     * What the app does in the background, shown as a small amber dot in the top-left corner (pulsing on colour
     * displays) that opens a card listing it. Hidden while the progress pill or the controls show.
     */
    BackgroundWork work;
    WorkCard workCard;
    bool workDotShown = false;
    uint32_t lastWorkPulse = 0;
    /** Where the position sits while navigating, as a share of the screen height from the top: more map ahead. */
    static constexpr double NAVIGATION_POSITION_Y = 0.72;
    static constexpr int WORK_DOT_X = 8;
    static constexpr int WORK_DOT_Y = 8;
    /** Whether drawStatus drew its pill this frame. */
    bool statusShown = false;
    void updateWork(uint32_t now);
    void drawWorkDot();
    /** Debug scripts: PSRAM held in 64 KB pieces until no free block is largest bytes, given back when the map closes. */
    void squeezeMemory(size_t largest);
    static constexpr int MAX_SQUEEZED = 64;
    void* squeezed[MAX_SQUEEZED] {};
    int squeezedCount = 0;
    /** Bit i: label font i has no cache file yet, so the maker has to make it. */
    unsigned missingLabelFonts = 0;
    bool labelFontsApplied = false;
    LabelStore labelStore;
    LabelLayout labels;
    bool labelsReady = false;
    /** The view the labels were placed for; they move with the map until it settles again. */
    double labelCenterX = 0.0;
    double labelCenterY = 0.0;
    int labelZoom = -1;
    uint32_t labelGeneration = 0;
    uint32_t lastLabelPlace = 0;
    MapOverlay overlay;
    Canvas canvas;
    UiTheme theme {};
    TileStore store;
    TileWorker worker;
    Navigator navigator;
    bool navigatorStarted = false;
    /** A debug script waits for the route search ("waitroute"). */
    bool waitingRoute = false;
    RouteCard routeCard;
    NavigationBar navBar;
    RouteFollower follower;
    bool navigating = false;
    bool recalculating = false;
    /** A search this view asked for (not a bench run) is running. */
    bool routeRequested = false;
    /** A mode was chosen while a search ran: a new search starts once it has stopped. */
    bool restartRoute = false;
    uint32_t lastRecalculation = 0;
    int64_t lastFollowFixMs = -1;
    uint32_t lastFollowAt = 0;
    uint32_t lastFollowSignature = 0;
    uint32_t arrivedAt = 0;
    double goalLat = 0.0;
    double goalLon = 0.0;
    RouteProfile routeProfile = RouteProfile::Walk;
    /** The start of the last route asked for, and whether it was the position. */
    double startLat = 0.0;
    double startLon = 0.0;
    bool startFromPosition = false;
    bool pickingStart = false;
    NavigationBar pickBar;
    /** Large: in PSRAM with the view. */
    ReplaySource replay;
    bool replaying = false;
    Downloader downloader;
    DebugScript script;
    Screen screen = Screen::Map;
    /** A full screen opened from the menu goes back to the menu, one opened from the map back to the map. */
    bool returnToMenu = false;
    /** The popup over the screen: the menu, its sub-menus, or the actions of a row in a list. */
    ListView menu;
    bool menuOpen = false;
    Popup popup = Popup::Main;
    static constexpr int MAX_POPUP_ACTIONS = 16;
    int popupActions[MAX_POPUP_ACTIONS] {};
    int popupActionCount = 0;
    HeaderLayout aboutHeader {};
    ListView regionList;
    RegionMeta regions[TileStore::MAX_REGIONS] {};
    int regionCount = 0;
    ListView settingList;
    ListView catalogList;
    char catalogPath[PATH_MAX_LENGTH] = "";
    FileBuffer catalogFile;
    RegionCatalog catalog;
    /** The region whose children the list shows, -1 for the continents; the row of "All of ..." when it has one. */
    int catalogParent = -1;
    int catalogRows[ListView::MAX_ITEMS] {};
    struct CatalogPlan {
        int region;
        int maxZoom;
        int64_t tiles;
        uint64_t bytes;
    };
    CatalogPlan catalogPlan {};

    Waypoints waypoints;
    /** The waypoint whose sheet is open, -1 for none or a new one. */
    int selectedWaypoint = -1;
    /** The waypoint a rename or delete is asked for, from the sheet or the list. */
    int editingWaypoint = -1;
    WaypointSheet sheet;
    SearchSheet searchSheet;
    SearchEngine* searchEngine = nullptr;
    SearchTask searchTask;
    SearchResult* searchResults = nullptr;
    int searchResultCount = 0;
    CardSearchFile* searchFiles[SEARCH_MAX_INDEXES] {};
    int searchFileCount = 0;
    RecentSearches* recentSearches = nullptr;
    /** The place of the last search result, marked on the map until another sheet opens. */
    double searchPinLat = 0.0;
    double searchPinLon = 0.0;
    bool searchPinShown = false;
    /** The places Enter walks through, nearest the centre first, and the one shown. */
    static constexpr int MAX_PLACE_ORDER = 16;
    int placeOrder[MAX_PLACE_ORDER] {};
    int placeOrderCount = 0;
    int placeOrderIndex = 0;
    double sheetLat = 0.0;
    double sheetLon = 0.0;
    ListView waypointList;
    int waypointRows[ListView::MAX_ITEMS] {};
    TrackRecorder recorder;
    ListView trackList;
    struct GpxFile {
        char name[48];
        bool shown;
        bool failed;
        GpxLayer* layer;
    };
    static constexpr int MAX_GPX_FILES = 16;
    GpxFile gpxFiles[MAX_GPX_FILES] {};
    int gpxFileCount = 0;
    ListView gpxList;
    GpxImport gpxImport;
    /** The offline search indexes, built in idle time (M6). */
    SearchIndexJob searchIndexJob;
    /** A build is due: at start, and after a download or an update ended. */
    bool searchIndexDue = true;
    /** No new build before this time after a failed one. */
    uint32_t searchIndexRetryAt = 0;
    bool waitingSearchIndex = false;
    Downloader::State searchIndexDownloadState = Downloader::State::Idle;
    void pollSearchIndex(uint32_t now);
    void startSearchIndex(bool force, const char* only = nullptr);
    void checkFile(const char* name);
    int gpxImporting = -1;
    char gpxFolder[PATH_MAX_LENGTH] = "";
    Dialog dialog;
    enum class DialogPurpose : uint8_t {
        Delete,
        Update,
        CatalogDownload,
        DeleteWaypoint,
        CancelDownload
    };
    DialogPurpose dialogPurpose = DialogPurpose::Delete;
    TextPrompt prompt;
    enum class PromptPurpose : uint8_t {
        RenameRegion,
        RenameWaypoint
    };
    PromptPurpose promptPurpose = PromptPurpose::RenameRegion;
    int aboutScroll = 0;
    int aboutLines = 0;
    char* aboutBuffer = nullptr;
    DownloadLayout downloadView {};
    Downloader::Status lastStatus {};
    /** The display title of the region in lastStatus, read from its meta.json when the region changes. */
    char statusTitle[48] = "";
    char statusTitleOf[REGION_NAME_MAX + 1] = "";
    /** The region of the download that was paused from the details card, so it can be resumed there. */
    char pausedRegion[REGION_NAME_MAX + 1] = "";
    char cancelRegion[REGION_NAME_MAX + 1] = "";
    TransferRate transferRate;
    DownloadCard downloadCard;
    Rect pillRect {};
    int browseDone = 0;
    int browseTotal = 0;
    bool lastBrowsing = false;
    int lastBrowseDone = 0;
    int lastBrowseTotal = 0;
    uint32_t lastStatusPoll = 0;
    uint32_t lastGapPoll = 0;
    uint32_t lastGapHint = 0;
    uint32_t messageUntil = 0;
    bool resumeChecked = false;
    /** A download that failed tries again at this time (0: none planned); retries counts the tries since the last success. */
    uint32_t retryAt = 0;
    int retries = 0;
    const Palette* palette = nullptr;
    bool monochrome = false;
    uint16_t* frame = nullptr;
    uint16_t* preview = nullptr;
    AffineScratch* scratch = nullptr;
    int width = 0;
    int height = 0;
    double centerX = 0.0;
    double centerY = 0.0;
    int zoom = 14;
    bool dirty = true;
    bool exitRequested = false;
    char rootPath[PATH_MAX_LENGTH] = "";
    TaskEventGroup eventGroup {};
    bool eventGroupReady = false;
    AppEventSubscription appEvents {};
    bool subscribed = false;
    Settings settings;
    StallProbe probe;
    SavedView savedView;
    bool hasSavedView = false;
    bool viewReady = false;
    LocationSource location;
    PositionTracker position;
    uint32_t lastLocationCheck = 0;
    char toastText[96] = "";
    uint32_t toastUntil = 0;
    bool following = false;
    /** While following: the map turns to the GPS course (smoothed, held while slow). */
    bool headingUp = false;
    HeadingFilter headingFilter;
    /** Degrees clockwise from north of the direction at the top of the screen, with its cosine and sine. */
    float bearing = 0.0f;
    float bearingCos = 1.0f;
    float bearingSin = 0.0f;
    /** Turning to a new bearing over TURN_ANIMATION_MS, drawn from cached tiles like the zoom animation. */
    struct TurnAnimation {
        bool active;
        float from;
        float to;
        uint32_t startMs;
        int frames;
        uint32_t composeMs;
    };
    TurnAnimation turn {};
    /**
     * A turned view is drawn nearest pixel while it moves; once it settles with all tiles ready, it is drawn again with
     * bilinear sampling, and stays so while this view (centre, zoom, bearing) is unchanged.
     */
    struct SmoothView {
        bool valid;
        double centerX;
        double centerY;
        int zoom;
        float bearing;
        bool logged;
    };
    SmoothView smooth {};
    bool isSmoothView() const;
    /** Text can be typed: on a keyboard, or on the on-screen keyboard of a touch screen. */
    bool canType() const { return input.hasKeyboard() || input.hasTouch(); }
    /** The on-screen keyboard shows while a text field is open on a board with a touch screen and no keyboard. */
    bool wantsTouchKeyboard() const;
    void syncTouchKeyboard();
    TouchKeyboard touchKeyboard;
    /** A touch played by a debug script (tap, hold, drag, pinch, twist), read instead of the touch screen. */
    DebugScript::Step syntheticTouch {};
    uint32_t syntheticTouchStart = 0;
    bool syntheticTouchActive = false;
    bool syntheticTouchEnded = false;
    void syntheticTouchState(uint32_t now, TouchState& state);
    void updateSmooth(uint32_t now);
    /** The bearing the labels were placed for. */
    float labelBearing = 0.0f;
    Attribution attribution;
    Rect attributionBar {};
    Controls controls;
    /** The pointer of a trackball in mouse mode, shown while the controls are. */
    bool cursorShown = false;
    /** When the pointer last moved; it hides after POINTER_HIDE_MS without movement. */
    uint32_t cursorMovedAt = 0;
    int cursorX = 0;
    int cursorY = 0;
    bool enterHeld = false;
    bool enterLong = false;
    uint32_t enterDownAt = 0;
    uint32_t enterSeenAt = 0;
    bool mouseHeld = false;
    uint32_t mouseDownAt = 0;
    bool mouseLong = false;
    /** The pointer moved while its button was held: the map was dragged, the release is no click. */
    bool mouseDragged = false;
    int mouseDragDistance = 0;
    int mouseRowAccumulator = 0;
    PointerAcceleration pointerAcceleration;
    Gestures gestures;
    int dragRowAccumulator = 0;
    bool pinching = false;
    /** Two-finger turning: starts only past PINCH_TURN_START degrees of twist, so a pinch does not turn the map. */
    bool pinchTurning = false;
    float pinchStartBearing = 0.0f;
    float pinchTwistStart = 0.0f;
    /** The world point at zoom under the fingers when the pinch started. */
    double pinchWorldX = 0.0;
    double pinchWorldY = 0.0;
    /** A zoom animation: the map at zoom (the target) magnified by a scale that eases from fromScale to 1. */
    struct ZoomAnimation {
        bool active;
        uint32_t startMs;
        float fromScale;
        float anchorX;
        float anchorY;
        double worldX;
        double worldY;
        int frames;
        uint32_t composeMs;
        uint32_t slowestFrameMs;
        /** Where the slowest frame spent its time. */
        uint32_t slowestLockMs;
        uint32_t slowestComposeMs;
        uint32_t frameLockMs;
        uint32_t frameComposeMs;
    };
    ZoomAnimation animation {};
    bool pinchStepped = false;
    float pinchScale = 1.0f;
    int pinchStartX = 0;
    int pinchStartY = 0;
    int pinchX = 0;
    int pinchY = 0;
    uint32_t lastInputAt = 0;
    uint32_t lastRecordingDraw = 0;
    bool updatesAllowed = false;
    uint32_t lastPowerCheck = 0;
    bool idle = false;
    uint32_t viewChangedAt = 0;
    bool browsingPrepared = false;
    uint32_t frameLockMs = 0;
    uint32_t frameComposeMs = 0;
    bool viewTimed = true;
    bool waitingIdle = false;
};

} // namespace maps
