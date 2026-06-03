#pragma once

#include <TactilityCpp/App.h>
#include <tt_app.h>
#include <lvgl.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "EpubService.h"

// Icon fonts (compiled in; small and device-independent)
LV_FONT_DECLARE(material_symbols_shared_20)
LV_FONT_DECLARE(material_symbols_shared_32)

// Font selection utility (defined in EpubReaderUI.cpp, shared across translation units).
const lv_font_t* selectContentFont(bool italic = false, bool bold = false);

class EpubReader final : public App {

    // App handle (stored for use in async rebuilds where AppHandle isn't available)
    AppHandle appHandle_ = nullptr;

    static constexpr size_t MAX_CHAPTER_HTML = 131072; // max HTML bytes read per chapter (128 KB)

    // EPUB / text reader state
    std::shared_ptr<EpubService> epub_;
    bool textMode_ = false;         // true when showing a plain .txt file (no epub_)
    std::string currentFilePath_;
    int currentSpineIndex_ = 0;
    AppLaunchId tocDialogId_    = 0;
    AppLaunchId psramAlertId_   = 0;  // Non-zero once the no-PSRAM alert has been launched
    int pendingChapterIndex_ = -1;  // Set by onResult, consumed in onShow

    // Per-chapter stripped plain text + current display offset
    std::string pageContent_;
    size_t      pageOffset_ = 0;

    // File browser state
    std::string dataRoot_;
    std::string browsePath_;
    std::string pendingFilePath_;   // Set before async open
    std::vector<std::pair<std::string, bool>> browserEntries_;  // {name, isDir}

    // Books folder
    std::string booksPath_;               // saved books folder path (empty = not set)
    int shelfPage_ = 0;                   // current page in shelf view (persists across open/close)

    // Background open token - incremented on each new open + in onHide to invalidate
    // any in-flight background task so its result is discarded if it arrives late.
    std::atomic<uint32_t> openToken_ = 0;

    // UI pointers (nulled in onHide)
    lv_obj_t* toolbar_        = nullptr;
    lv_obj_t* wrapperWidget_  = nullptr;
    // In text mode: an lv_label.
    // In EPUB mode: a transparent flex-column container holding per-paragraph labels.
    lv_obj_t* contentWidget_  = nullptr;

    // Font lifecycle - loads the 4 Noto Serif variants for the active display size tier
    // from binary assets via lv_binfont_create; unloaded in onHide (skipped during dialog roundtrips).
    void loadFonts();
    void unloadFonts();

    // UI builders
    void buildReaderUI(lv_obj_t* parent);
    // Parse an ESC-encoded page slice and populate contentWidget_ with labels.
    void renderSlice(const std::string& slice);
    void buildBrowserUI(lv_obj_t* parent);
    void buildShelfUI(lv_obj_t* parent);
    void setReaderToolbarButtons();
    void setBrowserToolbarButtons();

    // Chapter / text logic
    void loadChapter(int index, int direction = 1);
    void renderPage();
    void saveProgress();
    bool loadProgress(std::string& outPath, int& outChapter, bool& outIsText);
    void openTocDialog();

    // Helpers
    static std::string getAppDataRoot(AppHandle app);
    void loadBooksPath();
    void saveBooksPath();
    static bool isSupportedFile(const std::string& name);
    static bool isTextFile(const std::string& path);
    void scanBooksDir(const std::string& path, const std::string& prefix);  // shelf flat-scan helper

    // Async rebuilds - safe to call from within LVGL event callbacks
    static void asyncOpenEpub(void* data);
    static void asyncRestoreEpub(void* data);  // like asyncOpenEpub but keeps currentSpineIndex_
    static void asyncNavigateBrowser(void* data);
    static void asyncSwitchToBrowser(void* data);

    // Background open - runs EpubService::open off the LVGL task to avoid stack overflow.
    // asyncOpenComplete is posted via lv_async_call when done.
    static void spawnOpenTask(EpubReader* self, bool restore); // shared setup for both open paths
    static void backgroundOpenTask(void* data); // xTaskCreate target
    static void asyncOpenComplete(void* data);  // lv_async_call target, back on LVGL task
    static void asyncScrollToEnd(void* data);   // lv_async_call: scroll to bottom after layout

    // Navigation - shared by toolbar callbacks and tap zones
    void doPrev();
    void doNext();

    // LVGL event callbacks
    static void onPrevPressed(lv_event_t* e);
    static void onNextPressed(lv_event_t* e);
    static void onReaderTap(lv_event_t* e);
    static void onTocPressed(lv_event_t* e);
    static void onBrowsePressed(lv_event_t* e);
    static void onBrowserBack(lv_event_t* e);
    static void onBrowserItem(lv_event_t* e);
    static void onSetBooksFolder(lv_event_t* e);  // "Folder" toolbar button
    static void onShelfFirst(lv_event_t* e);
    static void onShelfPrev(lv_event_t* e);
    static void onShelfNext(lv_event_t* e);
    static void onShelfLast(lv_event_t* e);

public:
    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
    void onResult(AppHandle app, void* data, AppLaunchId launchId, AppResult result, BundleHandle resultData) override;
};
