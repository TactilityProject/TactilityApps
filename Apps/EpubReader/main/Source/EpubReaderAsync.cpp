#include "EpubReader.h"
#include <lvgl/widgets/toolbar.h>
#include <tactility/filesystem/file_mutex.h>
#include <tactility/log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <climits>
#include <cstdio>
#include <esp_heap_caps.h>

static const char* TAG = "EpubReader";

// ---------------------------------------------------------------------------
// Background open arguments - allocated on heap, owned by the background task,
// freed in asyncOpenComplete (or in spawnOpenTask on task-create failure).
// ---------------------------------------------------------------------------
struct OpenArgs {
    Context*                     ctx;
    std::string                  filePath;      // path to .epub or .txt
    bool                         restore;       // true = keep currentSpineIndex_ (restore mode)
    int                          spineIndex;    // savedChapter / savedOffset for restore
    uint32_t                     token;         // matches ctx->openToken_ at dispatch time
    // Results filled by backgroundOpenTask:
    std::shared_ptr<EpubService> epub;          // null on failure
    std::string                  textContent;   // pre-read content for .txt files
};

// ---------------------------------------------------------------------------
// Async rebuilds (deferred - safe to trigger from within LVGL callbacks)
// ---------------------------------------------------------------------------

// Helper: allocate an OpenArgs, clean the wrapper, show a placeholder, and spawn
// the background task.  Used by both asyncOpenEpub and asyncRestoreEpub.
void spawnOpenTask(Context* ctx, bool restore) {
    auto* args       = new OpenArgs{};
    args->ctx        = ctx;
    args->filePath   = ctx->pendingFilePath_;
    args->restore    = restore;
    args->spineIndex = ctx->currentSpineIndex_;
    args->token      = ctx->openToken_;

    // Show a brief placeholder so old content doesn't linger during the open
    lv_obj_clean(ctx->wrapperWidget_);
    lvgl_toolbar_clear_actions(ctx->toolbar_);
    lv_obj_t* lbl = lv_label_create(ctx->wrapperWidget_);
    lv_obj_set_style_pad_all(lbl, 8, 0);
    lv_label_set_text(lbl, restore ? "Loading..." : "Opening...");

    if (xTaskCreateWithCaps(backgroundOpenTask, "epubOpen", 32768 /* 32 KB */, args, 3, nullptr, MALLOC_CAP_SPIRAM)
            != pdPASS) {
        LOG_E(TAG, "Failed to create open task - out of memory");
        delete args;
        ctx->epub_ = nullptr;
        setBrowserToolbarButtons(ctx);
        lv_obj_clean(ctx->wrapperWidget_);
        buildBrowserUI(ctx, ctx->wrapperWidget_);
    }
}

// Like asyncOpenEpub but keeps currentSpineIndex_ - used when restoring a saved session.
void asyncRestoreEpub(void* data) {
    auto* ctx = static_cast<Context*>(data);
    if (!ctx->wrapperWidget_ || !ctx->toolbar_) return;
    ctx->textMode_ = false;
    ++ctx->openToken_;
    spawnOpenTask(ctx, /*restore=*/true);
}

void asyncNavigateBrowser(void* data) {
    auto* ctx = static_cast<Context*>(data);
    if (!ctx->wrapperWidget_ || !ctx->toolbar_) return;
    setBrowserToolbarButtons(ctx);
    lv_obj_clean(ctx->wrapperWidget_);
    buildBrowserUI(ctx, ctx->wrapperWidget_);
}

void asyncOpenEpub(void* data) {
    auto* ctx = static_cast<Context*>(data);
    if (!ctx->wrapperWidget_ || !ctx->toolbar_) return;

    ctx->currentSpineIndex_ = 0;
    ctx->textMode_          = false;
    ++ctx->openToken_;

    spawnOpenTask(ctx, /*restore=*/false);
}

void asyncSwitchToBrowser(void* data) {
    auto* ctx = static_cast<Context*>(data);
    if (!ctx->wrapperWidget_ || !ctx->toolbar_) return;

    ctx->epub_ = nullptr;
    ctx->textMode_          = false;
    ctx->currentSpineIndex_ = 0;
    ctx->contentWidget_      = nullptr;

    // Return to books folder (if set) so the user lands on their library
    if (!ctx->booksPath_.empty()) ctx->browsePath_ = ctx->booksPath_;
    setBrowserToolbarButtons(ctx);
    lv_obj_clean(ctx->wrapperWidget_);
    buildBrowserUI(ctx, ctx->wrapperWidget_);
}

// ---------------------------------------------------------------------------
// Background open task + completion callback
// ---------------------------------------------------------------------------

// Runs on a FreeRTOS task with its own 32 KB stack.
// Does all SD card I/O (epub parse or text file read) completely off the LVGL
// task to prevent stack overflow and serialise SDMMC access.
void backgroundOpenTask(void* data) {
    auto* a = static_cast<OpenArgs*>(data);

    // Acquire the filesystem lock before any SD card I/O - prevents concurrent
    // SDMMC access from the background and LVGL tasks (bus errors 0x107/0x108).
    struct FileMutex mutex;
    file_mutex_get(&mutex, a->filePath.c_str());
    file_mutex_lock(&mutex);

    if (isTextFile(a->filePath)) {
        // Read the entire text file here (under the lock) so asyncOpenComplete
        // only needs to update UI state - no SD I/O on the LVGL task.
        FILE* f = fopen(a->filePath.c_str(), "r");
        if (f) {
            char buf[512];
            while (a->textContent.size() < MAX_CHAPTER_HTML) {
                size_t remaining = MAX_CHAPTER_HTML - a->textContent.size();
                size_t toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
                size_t n = fread(buf, 1, toRead, f);
                if (n == 0) break;
                a->textContent.append(buf, n);
            }
            fclose(f);
        } else {
            LOG_E(TAG, "Cannot open text file: %s", a->filePath.c_str());
        }
    } else {
        // Open the epub (ZIP directory scan + OPF/NCX XML parse)
        a->epub = EpubService::open(a->filePath);
    }

    file_mutex_unlock(&mutex);

    // Signal the LVGL task that the work is done
    lv_async_call(asyncOpenComplete, a);
    vTaskDelete(nullptr);
}

// Called back on the LVGL task (via lv_async_call from backgroundOpenTask).
// Checks the open token, then either builds the reader UI or falls back to browser.
void asyncOpenComplete(void* data) {
    auto* a   = static_cast<OpenArgs*>(data);
    auto* ctx = a->ctx;

    // Discard stale results if the app was closed or a newer open was started
    if (!ctx->wrapperWidget_ || !ctx->toolbar_ || a->token != ctx->openToken_) {
        delete a;
        return;
    }

    if (isTextFile(a->filePath)) {
        ctx->epub_              = nullptr;
        ctx->textMode_          = false;
        ctx->currentSpineIndex_ = a->restore ? a->spineIndex : 0;
        if (!a->textContent.empty()) {
            // Content was pre-read in backgroundOpenTask (under the FS lock) -
            // no SD I/O needed here on the LVGL task.
            ctx->pageContent_      = std::move(a->textContent);
            ctx->textMode_         = true;
            ctx->currentFilePath_  = a->filePath;
            ctx->pageOffset_       = (ctx->currentSpineIndex_ > 0)
                                         ? (size_t)ctx->currentSpineIndex_ : 0u;
            ctx->currentSpineIndex_ = 0;
            LOG_I(TAG, "Text file loaded: %zu bytes", ctx->pageContent_.size());
        } else {
            // Text content empty (lock timeout or read error) - show error in browser
            LOG_E(TAG, "Text content empty; cannot display: %s", a->filePath.c_str());
            lv_obj_clean(ctx->wrapperWidget_);
            lv_obj_t* errLbl = lv_label_create(ctx->wrapperWidget_);
            lv_obj_set_style_pad_all(errLbl, 8, 0);
            lv_label_set_text(errLbl, "Failed to open file.\nPlease try again.");
            setBrowserToolbarButtons(ctx);
            delete a;
            return;
        }
        setReaderToolbarButtons(ctx);
        lv_obj_clean(ctx->wrapperWidget_);
        buildReaderUI(ctx, ctx->wrapperWidget_);
        delete a;
        return;
    }

    if (a->epub && a->epub->isValid()) {
        ctx->epub_            = a->epub;
        ctx->currentFilePath_ = a->filePath;
        setReaderToolbarButtons(ctx);
        lv_obj_clean(ctx->wrapperWidget_);
        buildReaderUI(ctx, ctx->wrapperWidget_);
    } else {
        LOG_E(TAG, "Failed to open: %s", a->filePath.c_str());
        ctx->epub_ = nullptr;
        ctx->currentSpineIndex_ = 0;
        setBrowserToolbarButtons(ctx);
        lv_obj_clean(ctx->wrapperWidget_);
        buildBrowserUI(ctx, ctx->wrapperWidget_);
    }
    delete a;
}

// ---------------------------------------------------------------------------
// LVGL event callbacks
// ---------------------------------------------------------------------------

// Fired via lv_async_call after renderPage() when loading a chapter backward (direction < 0).
// By the time this runs LVGL has completed layout, so content height is known and we can
// scroll to the very end - placing the user at the bottom of the chapter they backed into.
void asyncScrollToEnd(void* data) {
    auto* ctx = static_cast<Context*>(data);
    if (!ctx->contentWidget_ || !ctx->wrapperWidget_) return;
    lv_obj_t* scroll = lv_obj_get_parent(ctx->contentWidget_);
    if (!scroll) return;
    lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
    lv_coord_t sy = lv_obj_get_scroll_y(scroll);
    ctx->pageOffset_ = (sy > 0) ? (size_t)sy : 0;
    saveProgress(ctx);
}

// Snap a scroll step down to the nearest whole-line multiple so page turns
// always land on a clean line boundary (no partial lines at top or bottom).
// Uses the actual LVGL font line_height rather than hardcoded estimates.
static lv_coord_t snapStep(lv_coord_t viewH) {
    lv_coord_t lineH = (lv_coord_t)lv_font_get_line_height(selectContentFont());
    lv_coord_t step  = (lineH > 0) ? (viewH / lineH) * lineH : viewH;
    return (step > 0) ? step : viewH;  // fallback: scroll full height if tiny display
}

// Both text and EPUB modes use the same scroll-by-viewport-height approach.
// snapStep ensures each page turn is a whole-line multiple, so - provided all
// paragraph label Y positions are also multiples of lineH (zero label padding +
// pad_row=lineH on contentWidget_) - pages always start on a clean line boundary.
// At chapter boundaries (EPUB only) the adjacent chapter is loaded.
void doPrev(Context* ctx) {
    if (!ctx->contentWidget_) return;
    lv_obj_t* scroll = lv_obj_get_parent(ctx->contentWidget_);
    if (!scroll) return;
    lv_coord_t curY = lv_obj_get_scroll_y(scroll);
    lv_coord_t step = snapStep(lv_obj_get_height(scroll));
    lv_obj_scroll_to_y(scroll, curY > step ? curY - step : 0, LV_ANIM_OFF);
    lv_coord_t newY = lv_obj_get_scroll_y(scroll);
    if (newY == curY && !ctx->textMode_) {
        // Scroll didn't move - already at the top; cross into the previous chapter.
        if (ctx->currentSpineIndex_ > 0) loadChapter(ctx, ctx->currentSpineIndex_ - 1, -1);
    } else {
        ctx->pageOffset_ = (newY > 0) ? (size_t)newY : 0;
        saveProgress(ctx);
    }
}

void doNext(Context* ctx) {
    if (!ctx->contentWidget_) return;
    lv_obj_t* scroll = lv_obj_get_parent(ctx->contentWidget_);
    if (!scroll) return;
    lv_coord_t curY = lv_obj_get_scroll_y(scroll);
    lv_coord_t step = snapStep(lv_obj_get_height(scroll));
    lv_obj_scroll_to_y(scroll, curY + step, LV_ANIM_OFF);
    lv_coord_t newY = lv_obj_get_scroll_y(scroll);
    if (newY == curY && !ctx->textMode_) {
        // Scroll position didn't move - content fits in the viewport or we've reached
        // the end. lv_obj_get_scroll_bottom() returns negative for short chapters so
        // checking == 0 is unreliable; this approach works for all chapter lengths.
        loadChapter(ctx, ctx->currentSpineIndex_ + 1, +1);
    } else {
        ctx->pageOffset_ = (newY > 0) ? (size_t)newY : 0;
        saveProgress(ctx);
    }
}

void onPrevPressed(lv_event_t* e) {
    doPrev(static_cast<Context*>(lv_event_get_user_data(e)));
}

void onNextPressed(lv_event_t* e) {
    doNext(static_cast<Context*>(lv_event_get_user_data(e)));
}

void onReaderTap(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_coord_t w = lv_display_get_horizontal_resolution(nullptr);
    if (pt.x < w / 2) doPrev(ctx);
    else               doNext(ctx);
}

void onTocPressed(lv_event_t* e) {
    openTocDialog(static_cast<Context*>(lv_event_get_user_data(e)));
}

void onBrowsePressed(lv_event_t* e) {
    lv_async_call(asyncSwitchToBrowser, lv_event_get_user_data(e));
}

void onBrowserBack(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    size_t pos = ctx->browsePath_.rfind('/');
    if (pos != std::string::npos && ctx->browsePath_ != ctx->dataRoot_) {
        ctx->browsePath_ = ctx->browsePath_.substr(0, pos);
    }
    lv_async_call(asyncNavigateBrowser, ctx);
}

void onBrowserItem(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (idx >= ctx->browserEntries_.size()) return;

    const auto& [name, isDir] = ctx->browserEntries_[idx];
    if (isDir) {
        ctx->browsePath_ += "/" + name;
        lv_async_call(asyncNavigateBrowser, ctx);
    } else {
        ctx->pendingFilePath_ = ctx->browsePath_ + "/" + name;
        lv_async_call(asyncOpenEpub, ctx);
    }
}

// "Use Folder" toolbar button - save the current browsePath_ as the books folder.
void onSetBooksFolder(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    ctx->booksPath_ = ctx->browsePath_;
    saveBooksPath(ctx);
    lv_async_call(asyncNavigateBrowser, ctx);
}

// Shelf page navigation callbacks
void onShelfFirst(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    ctx->shelfPage_ = 0;
    lv_async_call(asyncNavigateBrowser, ctx);
}

void onShelfPrev(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (ctx->shelfPage_ > 0) --ctx->shelfPage_;
    lv_async_call(asyncNavigateBrowser, ctx);
}

void onShelfNext(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    ++ctx->shelfPage_;  // clamped in buildShelfUI
    lv_async_call(asyncNavigateBrowser, ctx);
}

void onShelfLast(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    ctx->shelfPage_ = INT_MAX;  // clamped to totalPages-1 in buildShelfUI
    lv_async_call(asyncNavigateBrowser, ctx);
}
