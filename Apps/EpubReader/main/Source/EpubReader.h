#pragma once

#include <atomic>
#include <lvgl.h>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

#include "EpubService.h"

// Icon fonts (compiled in; small and device-independent)
LV_FONT_DECLARE(material_symbols_shared_20)
LV_FONT_DECLARE(material_symbols_shared_32)

// Font selection utility (defined in EpubReaderUI.cpp, shared across translation units).
const lv_font_t* selectContentFont(bool italic = false, bool bold = false);

static constexpr size_t MAX_CHAPTER_HTML = 131072; // max HTML bytes read per chapter (128 KB)

struct Context {
    uint32_t appInstanceId;

    // EPUB / text reader state
    std::shared_ptr<EpubService> epub_;
    bool textMode_ = false;         // true when showing a plain .txt file (no epub_)
    std::string currentFilePath_;
    int currentSpineIndex_ = 0;
    uint32_t tocDialogId_    = 0;
    uint32_t psramAlertId_   = 0;  // Non-zero once the no-PSRAM alert has been launched

    // Per-chapter stripped plain text + current display offset
    std::string pageContent_;
    size_t      pageOffset_ = 0;

    // File browser state
    std::string dataRoot_;
    std::string browsePath_;
    std::string pendingFilePath_;   // Set before async open
    // Path passed on the command line at launch (argv[0]), or empty. Re-checked (and left set,
    // matching the original bundle-based behavior of re-checking every show) each time
    // createWidgets() runs until an epub_ is successfully opened.
    std::string launchFilePath_;
    std::vector<std::pair<std::string, bool>> browserEntries_;  // {name, isDir}

    // Books folder
    std::string booksPath_;               // saved books folder path (empty = not set)
    int shelfPage_ = 0;                   // current page in shelf view (persists across open/close)

    // Background open token - incremented in epubReaderTeardown() to invalidate any in-flight
    // background task so its result is discarded if it arrives after the app has closed.
    std::atomic<uint32_t> openToken_ = 0;

    // UI pointers (nulled in epubReaderTeardown())
    lv_obj_t* toolbar_        = nullptr;
    lv_obj_t* wrapperWidget_  = nullptr;
    // In text mode: an lv_label.
    // In EPUB mode: a transparent flex-column container holding per-paragraph labels.
    lv_obj_t* contentWidget_  = nullptr;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void epubReaderCreateWidgets(lv_obj_t* parent, void* userData);

/** Releases fonts and joins/invalidates any in-flight background job. Call once the window is torn down. */
void epubReaderTeardown(Context* ctx);

// ---------------------------------------------------------------------------
// Shared across translation units (EpubReader.cpp / EpubReaderUI.cpp / EpubReaderAsync.cpp)
// ---------------------------------------------------------------------------

// Font lifecycle - loads the 4 Noto Serif variants for the active display size tier
// from binary assets via lv_binfont_create; unloaded in epubReaderTeardown().
void loadFonts(Context* ctx);
void unloadFonts();

// UI builders
void buildReaderUI(Context* ctx, lv_obj_t* parent);
// Parse an ESC-encoded page slice and populate contentWidget_ with labels.
void renderSlice(Context* ctx, const std::string& slice);
void buildBrowserUI(Context* ctx, lv_obj_t* parent);
void buildShelfUI(Context* ctx, lv_obj_t* parent);
void setReaderToolbarButtons(Context* ctx);
void setBrowserToolbarButtons(Context* ctx);

// Chapter / text logic
void loadChapter(Context* ctx, int index, int direction = 1);
void renderPage(Context* ctx);
void saveProgress(Context* ctx);
bool loadProgress(std::string& outPath, int& outChapter, bool& outIsText);
void openTocDialog(Context* ctx);

// Helpers
void loadBooksPath(Context* ctx);
void saveBooksPath(Context* ctx);
bool isSupportedFile(const std::string& name);
bool isTextFile(const std::string& path);
void scanBooksDir(Context* ctx, const std::string& path, const std::string& prefix);  // shelf flat-scan helper

// Async rebuilds - safe to call from within LVGL event callbacks
void asyncOpenEpub(void* data);
void asyncRestoreEpub(void* data);  // like asyncOpenEpub but keeps currentSpineIndex_
void asyncNavigateBrowser(void* data);
void asyncSwitchToBrowser(void* data);

// Background open - runs EpubService::open off the LVGL task to avoid stack overflow.
// asyncOpenComplete is posted via lv_async_call when done.
void spawnOpenTask(Context* ctx, bool restore); // shared setup for both open paths
void backgroundOpenTask(void* data); // xTaskCreate target
void asyncOpenComplete(void* data);  // lv_async_call target, back on LVGL task
void asyncScrollToEnd(void* data);   // lv_async_call: scroll to bottom after layout

// Navigation - shared by toolbar callbacks and tap zones
void doPrev(Context* ctx);
void doNext(Context* ctx);

// LVGL event callbacks
void onPrevPressed(lv_event_t* e);
void onNextPressed(lv_event_t* e);
void onReaderTap(lv_event_t* e);
void onTocPressed(lv_event_t* e);
void onBrowsePressed(lv_event_t* e);
void onBrowserBack(lv_event_t* e);
void onBrowserItem(lv_event_t* e);
void onSetBooksFolder(lv_event_t* e);  // "Folder" toolbar button
void onShelfFirst(lv_event_t* e);
void onShelfPrev(lv_event_t* e);
void onShelfNext(lv_event_t* e);
void onShelfLast(lv_event_t* e);
