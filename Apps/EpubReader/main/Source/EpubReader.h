#pragma once

#include <tactility/concurrent/task_event_group.h>

#include <atomic>
#include <lvgl.h>
#include <memory>
#include <stdint.h>
#include <string>
#include <vector>

#include "EpubService.h"

// Font selection utility (defined in EpubReaderUI.cpp, shared across translation units).
const lv_font_t* selectContentFont(bool italic = false, bool bold = false);

static constexpr size_t MAX_CHAPTER_HTML = 131072; // max HTML bytes read per chapter (128 KB)
static constexpr size_t MAX_RECENT_BOOKS = 20;

/** A book that was opened before, and where reading stopped */
struct RecentBook {
    std::string path;
    std::string title;
    int chapter = 0;     // EPUB spine index
    int offset = 0;      // scroll position within the chapter or text file
    int percent = 0;
};

enum class View { Library, Loading, Reader };

/** The dialog that is shown, if any */
enum class ReaderDialog { None, Menu, Contents };

enum class MenuAction { Contents, OpenFile, CloseBook };

struct Context {
    uint32_t appInstanceId;
    View view = View::Library;
    /** Most recently opened first. Guarded by the LVGL lock. */
    std::vector<RecentBook> recents_;
    std::string libraryError_;

    // EPUB / text reader state
    std::shared_ptr<EpubService> epub_;
    bool textMode_ = false;         // true when showing a plain .txt file (no epub_)
    std::string currentFilePath_;
    int currentSpineIndex_ = 0;

    // Per-chapter stripped plain text + scroll position within it
    std::string pageContent_;
    size_t      pageOffset_ = 0;

    // Dialog results and the like are handled on the app task, which these bits wake
    TaskEventGroup* eventGroup_ = nullptr;
    uint32_t dialogBit_ = 0;
    uint32_t openFileBit_ = 0;
    uint32_t psramAlertBit_ = 0;
    std::atomic<ReaderDialog> dialog_ { ReaderDialog::None };
    std::atomic<int32_t> dialogResult_ { -1 };
    std::vector<MenuAction> menuActions_;

    // The file dialog. Its result is handled on the app task, which fileBit_ wakes.
    uint32_t fileBit_ = 0;
    std::string pickedPath_;

    // Background open token - incremented to invalidate any in-flight background task so its
    // result is discarded when it arrives after the user went elsewhere or the app closed.
    std::atomic<uint32_t> openToken_ = 0;

    // UI pointers (nulled when the widgets are destroyed)
    lv_obj_t* toolbar_        = nullptr;
    lv_obj_t* wrapperWidget_  = nullptr;
    // In text mode: an lv_label.
    // In EPUB mode: a transparent flex-column container holding per-paragraph labels.
    lv_obj_t* contentWidget_  = nullptr;
    // The page and progress line below the text
    lv_obj_t* statusLabel_    = nullptr;
};

/** window_manager_create_ext()'s callbacks - @a userData is the Context* for this instance. */
void epubReaderCreateWidgets(lv_obj_t* parent, void* userData);
void epubReaderDestroyWidgets(void* userData);

/** Loads the recent books. Call on the app task, before the window is created. */
void epubReaderInit(Context* ctx);
/** Releases fonts and invalidates any in-flight background job. Call once the window is torn down. */
void epubReaderTeardown(Context* ctx);

// Call on the app task, without the LVGL lock
void startFileSelection(Context* ctx);
void handleFileResult(Context* ctx);
void handleDialogResult(Context* ctx);

// The functions below require the LVGL lock

void loadFonts(Context* ctx);
void unloadFonts();

/** Rebuilds the toolbar and the content for ctx->view */
void showView(Context* ctx);
/** Parse an ESC-encoded page slice and populate contentWidget_ with labels. */
void renderSlice(Context* ctx, const std::string& slice);

void loadChapter(Context* ctx, int index, int direction = 1);
void renderPage(Context* ctx);
/** Moves the current book to the front of the recent books, with the current position */
void saveProgress(Context* ctx);
void writeRecents(Context* ctx);

bool isTextFile(const std::string& path);

/** Starts opening a book on a background task, at the position it was left */
void openBook(Context* ctx, const std::string& path);
void closeBook(Context* ctx);
/** Saves where reading is, also after scrolling by dragging */
void rememberPosition(Context* ctx);

void asyncShowView(void* data);   // lv_async_call target, for rebuilds from widget events
void asyncOpenComplete(void* data);
void asyncScrollToEnd(void* data);
void backgroundOpenTask(void* data);

/** How far into the book the reader is, 0 to 100. Requires contentWidget_. */
int readingPercent(Context* ctx);
/** Shows the page within the chapter and the progress */
void updateStatus(Context* ctx, int percent);

void doPrev(Context* ctx);
void doNext(Context* ctx);
