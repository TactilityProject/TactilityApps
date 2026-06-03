#include "EpubReader.h"
#include <tt_lvgl_toolbar.h>
#include <tt_lock.h>
#include <tactility/log.h>
#include <Tactility/kernel/Kernel.h>
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
    EpubReader*                  self;
    std::string                  filePath;      // path to .epub or .txt
    bool                         restore;       // true = keep currentSpineIndex_ (restore mode)
    int                          spineIndex;    // savedChapter / savedOffset for restore
    uint32_t                     token;         // matches self->openToken_ at dispatch time
    // Results filled by backgroundOpenTask:
    std::shared_ptr<EpubService> epub;          // null on failure
    std::string                  textContent;   // pre-read content for .txt files
};

// ---------------------------------------------------------------------------
// Async rebuilds (deferred - safe to trigger from within LVGL callbacks)
// ---------------------------------------------------------------------------

// Helper: allocate an OpenArgs, clean the wrapper, show a placeholder, and spawn
// the background task.  Used by both asyncOpenEpub and asyncRestoreEpub.
void EpubReader::spawnOpenTask(EpubReader* self, bool restore) {
    auto* args       = new OpenArgs{};
    args->self       = self;
    args->filePath   = self->pendingFilePath_;
    args->restore    = restore;
    args->spineIndex = self->currentSpineIndex_;
    args->token      = self->openToken_;

    // Show a brief placeholder so old content doesn't linger during the open
    lv_obj_clean(self->wrapperWidget_);
    tt_lvgl_toolbar_clear_actions(self->toolbar_);
    lv_obj_t* lbl = lv_label_create(self->wrapperWidget_);
    lv_obj_set_style_pad_all(lbl, 8, 0);
    lv_label_set_text(lbl, restore ? "Loading..." : "Opening...");

    if (xTaskCreateWithCaps(EpubReader::backgroundOpenTask, "epubOpen", 32768 /* 32 KB */, args, 3, nullptr, MALLOC_CAP_SPIRAM)
            != pdPASS) {
        LOG_E(TAG, "Failed to create open task - out of memory");
        delete args;
        self->epub_ = nullptr;
        self->setBrowserToolbarButtons();
        lv_obj_clean(self->wrapperWidget_);
        self->buildBrowserUI(self->wrapperWidget_);
    }
}

// Like asyncOpenEpub but keeps currentSpineIndex_ - used when restoring a saved session.
void EpubReader::asyncRestoreEpub(void* data) {
    auto* self = static_cast<EpubReader*>(data);
    if (!self->wrapperWidget_ || !self->toolbar_) return;
    self->textMode_ = false;
    ++self->openToken_;
    spawnOpenTask(self, /*restore=*/true);
}

void EpubReader::asyncNavigateBrowser(void* data) {
    auto* self = static_cast<EpubReader*>(data);
    if (!self->wrapperWidget_ || !self->toolbar_) return;
    self->setBrowserToolbarButtons();
    lv_obj_clean(self->wrapperWidget_);
    self->buildBrowserUI(self->wrapperWidget_);
}

void EpubReader::asyncOpenEpub(void* data) {
    auto* self = static_cast<EpubReader*>(data);
    if (!self->wrapperWidget_ || !self->toolbar_) return;

    self->currentSpineIndex_ = 0;
    self->textMode_          = false;
    ++self->openToken_;

    spawnOpenTask(self, /*restore=*/false);
}

void EpubReader::asyncSwitchToBrowser(void* data) {
    auto* self = static_cast<EpubReader*>(data);
    if (!self->wrapperWidget_ || !self->toolbar_) return;

    self->epub_ = nullptr;
    self->textMode_          = false;
    self->currentSpineIndex_ = 0;
    self->contentWidget_      = nullptr;

    // Return to books folder (if set) so the user lands on their library
    if (!self->booksPath_.empty()) self->browsePath_ = self->booksPath_;
    self->setBrowserToolbarButtons();
    lv_obj_clean(self->wrapperWidget_);
    self->buildBrowserUI(self->wrapperWidget_);
}

// ---------------------------------------------------------------------------
// Background open task + completion callback
// ---------------------------------------------------------------------------

// Runs on a FreeRTOS task with its own 32 KB stack.
// Does all SD card I/O (epub parse or text file read) completely off the LVGL
// task to prevent stack overflow and serialise SDMMC access.
void EpubReader::backgroundOpenTask(void* data) {
    auto* a = static_cast<OpenArgs*>(data);

    // Acquire the filesystem lock before any SD card I/O - prevents concurrent
    // SDMMC access from the background and LVGL tasks (bus errors 0x107/0x108).
    auto lock = tt_lock_alloc_for_path(a->filePath.c_str());
    if (!tt_lock_acquire(lock, tt::kernel::MAX_TICKS)) {
        LOG_E(TAG, "FS lock timed out, skipping open: %s", a->filePath.c_str());
        tt_lock_free(lock);
        lv_async_call(asyncOpenComplete, a);
        vTaskDelete(nullptr);
        return;
    }

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

    tt_lock_release(lock);
    tt_lock_free(lock);

    // Signal the LVGL task that the work is done
    lv_async_call(asyncOpenComplete, a);
    vTaskDelete(nullptr);
}

// Called back on the LVGL task (via lv_async_call from backgroundOpenTask).
// Checks the open token, then either builds the reader UI or falls back to browser.
void EpubReader::asyncOpenComplete(void* data) {
    auto* a    = static_cast<OpenArgs*>(data);
    auto* self = a->self;

    // Discard stale results if the app was hidden or a newer open was started
    if (!self->wrapperWidget_ || !self->toolbar_ || a->token != self->openToken_) {
        delete a;
        return;
    }

    if (isTextFile(a->filePath)) {
        self->epub_              = nullptr;
        self->textMode_          = false;
        self->currentSpineIndex_ = a->restore ? a->spineIndex : 0;
        if (!a->textContent.empty()) {
            // Content was pre-read in backgroundOpenTask (under the FS lock) -
            // no SD I/O needed here on the LVGL task.
            self->pageContent_      = std::move(a->textContent);
            self->textMode_         = true;
            self->currentFilePath_  = a->filePath;
            self->pageOffset_       = (self->currentSpineIndex_ > 0)
                                         ? (size_t)self->currentSpineIndex_ : 0u;
            self->currentSpineIndex_ = 0;
            LOG_I(TAG, "Text file loaded: %zu bytes", self->pageContent_.size());
        } else {
            // Text content empty (lock timeout or read error) - show error in browser
            LOG_E(TAG, "Text content empty; cannot display: %s", a->filePath.c_str());
            lv_obj_clean(self->wrapperWidget_);
            lv_obj_t* errLbl = lv_label_create(self->wrapperWidget_);
            lv_obj_set_style_pad_all(errLbl, 8, 0);
            lv_label_set_text(errLbl, "Failed to open file.\nPlease try again.");
            self->setBrowserToolbarButtons();
            delete a;
            return;
        }
        self->setReaderToolbarButtons();
        lv_obj_clean(self->wrapperWidget_);
        self->buildReaderUI(self->wrapperWidget_);
        delete a;
        return;
    }

    if (a->epub && a->epub->isValid()) {
        self->epub_            = a->epub;
        self->currentFilePath_ = a->filePath;
        self->setReaderToolbarButtons();
        lv_obj_clean(self->wrapperWidget_);
        self->buildReaderUI(self->wrapperWidget_);
    } else {
        LOG_E(TAG, "Failed to open: %s", a->filePath.c_str());
        self->epub_ = nullptr;
        self->currentSpineIndex_ = 0;
        self->setBrowserToolbarButtons();
        lv_obj_clean(self->wrapperWidget_);
        self->buildBrowserUI(self->wrapperWidget_);
    }
    delete a;
}

// ---------------------------------------------------------------------------
// LVGL event callbacks
// ---------------------------------------------------------------------------

// Fired via lv_async_call after renderPage() when loading a chapter backward (direction < 0).
// By the time this runs LVGL has completed layout, so content height is known and we can
// scroll to the very end - placing the user at the bottom of the chapter they backed into.
void EpubReader::asyncScrollToEnd(void* data) {
    auto* self = static_cast<EpubReader*>(data);
    if (!self->contentWidget_ || !self->wrapperWidget_) return;
    lv_obj_t* scroll = lv_obj_get_parent(self->contentWidget_);
    if (!scroll) return;
    lv_obj_scroll_to_y(scroll, LV_COORD_MAX, LV_ANIM_OFF);
    lv_coord_t sy = lv_obj_get_scroll_y(scroll);
    self->pageOffset_ = (sy > 0) ? (size_t)sy : 0;
    self->saveProgress();
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
void EpubReader::doPrev() {
    if (!contentWidget_) return;
    lv_obj_t* scroll = lv_obj_get_parent(contentWidget_);
    if (!scroll) return;
    lv_coord_t curY = lv_obj_get_scroll_y(scroll);
    lv_coord_t step = snapStep(lv_obj_get_height(scroll));
    lv_obj_scroll_to_y(scroll, curY > step ? curY - step : 0, LV_ANIM_OFF);
    lv_coord_t newY = lv_obj_get_scroll_y(scroll);
    if (newY == curY && !textMode_) {
        // Scroll didn't move - already at the top; cross into the previous chapter.
        if (currentSpineIndex_ > 0) loadChapter(currentSpineIndex_ - 1, -1);
    } else {
        pageOffset_ = (newY > 0) ? (size_t)newY : 0;
        saveProgress();
    }
}

void EpubReader::doNext() {
    if (!contentWidget_) return;
    lv_obj_t* scroll = lv_obj_get_parent(contentWidget_);
    if (!scroll) return;
    lv_coord_t curY = lv_obj_get_scroll_y(scroll);
    lv_coord_t step = snapStep(lv_obj_get_height(scroll));
    lv_obj_scroll_to_y(scroll, curY + step, LV_ANIM_OFF);
    lv_coord_t newY = lv_obj_get_scroll_y(scroll);
    if (newY == curY && !textMode_) {
        // Scroll position didn't move - content fits in the viewport or we've reached
        // the end. lv_obj_get_scroll_bottom() returns negative for short chapters so
        // checking == 0 is unreliable; this approach works for all chapter lengths.
        loadChapter(currentSpineIndex_ + 1, +1);
    } else {
        pageOffset_ = (newY > 0) ? (size_t)newY : 0;
        saveProgress();
    }
}

void EpubReader::onPrevPressed(lv_event_t* e) {
    static_cast<EpubReader*>(lv_event_get_user_data(e))->doPrev();
}

void EpubReader::onNextPressed(lv_event_t* e) {
    static_cast<EpubReader*>(lv_event_get_user_data(e))->doNext();
}

void EpubReader::onReaderTap(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    lv_coord_t w = lv_display_get_horizontal_resolution(nullptr);
    if (pt.x < w / 2) self->doPrev();
    else               self->doNext();
}

void EpubReader::onTocPressed(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    self->openTocDialog();
}

void EpubReader::onBrowsePressed(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    lv_async_call(asyncSwitchToBrowser, self);
}

void EpubReader::onBrowserBack(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    size_t pos = self->browsePath_.rfind('/');
    if (pos != std::string::npos && self->browsePath_ != self->dataRoot_) {
        self->browsePath_ = self->browsePath_.substr(0, pos);
    }
    lv_async_call(asyncNavigateBrowser, self);
}

void EpubReader::onBrowserItem(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    uintptr_t idx = (uintptr_t)lv_obj_get_user_data(lv_event_get_target_obj(e));
    if (idx >= self->browserEntries_.size()) return;

    const auto& [name, isDir] = self->browserEntries_[idx];
    if (isDir) {
        self->browsePath_ += "/" + name;
        lv_async_call(asyncNavigateBrowser, self);
    } else {
        self->pendingFilePath_ = self->browsePath_ + "/" + name;
        lv_async_call(asyncOpenEpub, self);
    }
}

// "Use Folder" toolbar button - save the current browsePath_ as the books folder.
void EpubReader::onSetBooksFolder(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    self->booksPath_ = self->browsePath_;
    self->saveBooksPath();
    lv_async_call(asyncNavigateBrowser, self);
}

// Shelf page navigation callbacks
void EpubReader::onShelfFirst(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    self->shelfPage_ = 0;
    lv_async_call(asyncNavigateBrowser, self);
}

void EpubReader::onShelfPrev(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    if (self->shelfPage_ > 0) --self->shelfPage_;
    lv_async_call(asyncNavigateBrowser, self);
}

void EpubReader::onShelfNext(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    ++self->shelfPage_;  // clamped in buildShelfUI
    lv_async_call(asyncNavigateBrowser, self);
}

void EpubReader::onShelfLast(lv_event_t* e) {
    auto* self = static_cast<EpubReader*>(lv_event_get_user_data(e));
    self->shelfPage_ = INT_MAX;  // clamped to totalPages-1 in buildShelfUI
    lv_async_call(asyncNavigateBrowser, self);
}
