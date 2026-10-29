// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "BinFontText.h"
#include "Debug.h"
#include "DisplayOutput.h"
#include "Downloader.h"
#include "Input.h"
#include "IoTask.h"
#include "LocationSource.h"
#include "StallProbe.h"
#include "TileWorker.h"

#include "../tiles/DownloadPlan.h"
#include "../tiles/RegionMeta.h"
#include "../tiles/TileStore.h"
#include "../ui/Canvas.h"
#include "../ui/Controls.h"
#include "../ui/DownloadFrame.h"
#include "../ui/Gestures.h"
#include "../ui/MapOverlay.h"
#include "../ui/PositionTracker.h"
#include "../ui/Settings.h"
#include "../ui/Widgets.h"

#include <app/event.h>
#include <tactility/device.h>

#include <cstdint>

namespace maps {

#define APP_VERSION "0.0.1"

/**
 * The whole app while it runs fullscreen with LVGL stopped: the map, the menu, the Regions, Settings and About
 * screens, and the download frame, all drawn by the app itself.
 */
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
        About
    };

    enum class MenuItem : uint8_t {
        Download,
        Regions,
        Settings,
        About,
        Exit,
        Count
    };

    enum class SettingItem : uint8_t {
        SdCache,
        PreparedTiles,
        SmoothPinch,
        BrowseDownload,
        Count
    };

    // MapView.cpp: the map itself
    void centerOn(double lat, double lon, int zoom);
    /** Remembers the centre and zoom, so the map opens there next time. */
    void saveView();
    void setZoom(int zoom, int anchorX, int anchorY);
    void pan(double dx, double dy);
    void updateWanted();
    void render();
    void drawMap();
    void drawStatus();
    void pollDownloader(uint32_t now);
    /** Hands the tiles that lack detail to the downloader, or says why they stay coarse. */
    void pollGaps(uint32_t now);
    Rect attributionMark() const;
    void readLocation();
    void showToast(const char* text);
    void startFollowing();
    bool markerScreenPosition(int& x, int& y) const;
    void saveShot(const char* name);
    bool isViewComplete();
    static const uint16_t* lookupTile(const TileKey& tile, void* context);
    const uint16_t* findTile(const TileKey& tile);
    void endPinch(float scale, int fromX, int fromY, int toX, int toY);
    bool isFullScreen() const { return screen == Screen::Regions || screen == Screen::Settings || screen == Screen::About; }

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
    void pressControl(Control control);
    /** A tap or pointer click on the map: buttons and the attribution first. Returns true when something took it. */
    bool pressAt(int x, int y);
    PositionButton positionButton() const;

    // MapScreens.cpp: menu, Regions, Settings, About, download frame
    void openMenu();
    void closeMenu();
    void chooseMenu(MenuItem item);
    void handleListAction(UiAction action);
    void openRegions();
    void fillRegionList();
    void chooseRegion();
    void askDeleteRegion();
    void deleteRegion();
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
    MapOverlay overlay;
    Canvas canvas;
    UiTheme theme {};
    TileStore store;
    TileWorker worker;
    Downloader downloader;
    DebugScript script;
    Screen screen = Screen::Map;
    ListView menu;
    bool menuOpen = false;
    ListView regionList;
    RegionMeta regions[TileStore::MAX_REGIONS] {};
    int regionCount = 0;
    ListView settingList;
    Dialog dialog;
    int aboutScroll = 0;
    int aboutLines = 0;
    char* aboutBuffer = nullptr;
    DownloadLayout downloadView {};
    Downloader::Status lastStatus {};
    uint32_t lastStatusPoll = 0;
    uint32_t lastGapPoll = 0;
    uint32_t lastGapHint = 0;
    uint32_t messageUntil = 0;
    bool resumeChecked = false;
    const Palette* palette = nullptr;
    bool monochrome = false;
    uint16_t* frame = nullptr;
    uint16_t* preview = nullptr;
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
    Attribution attribution;
    Rect attributionBar {};
    Controls controls;
    /** The pointer of a trackball in mouse mode, shown while the controls are. */
    bool cursorShown = false;
    int cursorX = 0;
    int cursorY = 0;
    bool enterHeld = false;
    bool enterLong = false;
    uint32_t enterDownAt = 0;
    uint32_t enterSeenAt = 0;
    bool mouseHeld = false;
    uint32_t mouseDownAt = 0;
    bool mouseLong = false;
    int mouseRowAccumulator = 0;
    Gestures gestures;
    int dragRowAccumulator = 0;
    uint16_t* pinchBase = nullptr;
    bool pinching = false;
    bool pinchStepped = false;
    float pinchScale = 1.0f;
    int pinchStartX = 0;
    int pinchStartY = 0;
    int pinchX = 0;
    int pinchY = 0;
    uint32_t lastInputAt = 0;
    bool idle = false;
    uint32_t viewChangedAt = 0;
    bool browsingPrepared = false;
    uint32_t frameLockMs = 0;
    uint32_t frameComposeMs = 0;
    bool viewTimed = true;
    bool waitingIdle = false;
};

} // namespace maps
