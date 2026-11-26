#include "EpubReader.h"
#include <tactility/log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <cstdio>
#include <esp_heap_caps.h>

static const char* TAG = "EpubReader";

// ---------------------------------------------------------------------------
// Background open arguments - allocated on heap, owned by the background task,
// freed in asyncOpenComplete (or in openBook on task-create failure).
// ---------------------------------------------------------------------------
struct OpenArgs {
    Context*                     ctx;
    std::string                  filePath;      // path to .epub or .txt
    uint32_t                     token;         // matches ctx->openToken_ at dispatch time
    // Results filled by backgroundOpenTask:
    std::shared_ptr<EpubService> epub;          // null on failure
    std::string                  textContent;   // pre-read content for .txt files
};

// ---------------------------------------------------------------------------
// Opening and closing books
// ---------------------------------------------------------------------------

static void failOpen(Context* ctx, const std::string& path) {
    LOG_E(TAG, "Failed to open: %s", path.c_str());
    ctx->libraryError_ = "Can't open " + path.substr(path.find_last_of('/') + 1);
    // The book stays in the recent books: the failure may be temporary, and its position is there
    ctx->view = View::Library;
}

void rememberPosition(Context* ctx) {
    if (ctx->view != View::Reader || ctx->contentWidget_ == nullptr) return;
    const int32_t y = lv_obj_get_scroll_y(lv_obj_get_parent(ctx->contentWidget_));
    ctx->pageOffset_ = y > 0 ? static_cast<size_t>(y) : 0;
    saveProgress(ctx);
}

void closeBook(Context* ctx) {
    rememberPosition(ctx);
    ++ctx->openToken_;
    ctx->contentWidget_ = nullptr;
    ctx->currentFilePath_.clear();
    ctx->epub_ = nullptr;
    ctx->textMode_ = false;
    ctx->pageContent_ = {};
    ctx->pageOffset_ = 0;
    ctx->currentSpineIndex_ = 0;
    ctx->view = View::Library;
}

void openBook(Context* ctx, const std::string& path) {
    // The path may be one of a recent book, which closing or opening can change
    const std::string bookPath = path;
    closeBook(ctx);
    ctx->libraryError_.clear();

    for (const auto& book : ctx->recents_) {
        if (book.path == bookPath) {
            ctx->currentSpineIndex_ = book.chapter;
            ctx->pageOffset_ = book.offset > 0 ? static_cast<size_t>(book.offset) : 0;
            break;
        }
    }

    auto* args     = new OpenArgs{};
    args->ctx      = ctx;
    args->filePath = bookPath;
    args->token    = ctx->openToken_;
    ctx->view = View::Loading;

    if (xTaskCreateWithCaps(backgroundOpenTask, "epubOpen", 32768 /* 32 KB */, args, 3, nullptr, MALLOC_CAP_SPIRAM)
            != pdPASS) {
        LOG_E(TAG, "Failed to create open task - out of memory");
        delete args;
        ctx->libraryError_ = "Not enough memory to open the book";
        ctx->view = View::Library;
    }
}

// Runs on a FreeRTOS task with its own 32 KB stack.
// Does all SD card I/O (epub parse or text file read) completely off the LVGL
// task to prevent stack overflow and serialise SDMMC access.
void backgroundOpenTask(void* data) {
    auto* a = static_cast<OpenArgs*>(data);

    if (isTextFile(a->filePath)) {
        // Read the entire text file here so asyncOpenComplete only needs to
        // update UI state - no SD I/O on the LVGL task.
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

    // Signal the LVGL task that the work is done
    lv_async_call(asyncOpenComplete, a);
    vTaskDelete(nullptr);
}

// Called back on the LVGL task (via lv_async_call from backgroundOpenTask).
void asyncOpenComplete(void* data) {
    auto* a   = static_cast<OpenArgs*>(data);
    auto* ctx = a->ctx;

    // Discard stale results if the user went elsewhere or a newer open was started
    if (a->token != ctx->openToken_) {
        delete a;
        return;
    }

    if (isTextFile(a->filePath) && !a->textContent.empty()) {
        ctx->textMode_ = true;
        ctx->currentSpineIndex_ = 0;
        ctx->pageContent_ = std::move(a->textContent);
        ctx->currentFilePath_ = a->filePath;
        ctx->view = View::Reader;
    } else if (a->epub && a->epub->isValid()) {
        ctx->epub_ = a->epub;
        ctx->currentFilePath_ = a->filePath;
        ctx->view = View::Reader;
    } else {
        failOpen(ctx, a->filePath);
    }
    delete a;
    // Without widgets, the view is shown when they are created
    showView(ctx);
}

// ---------------------------------------------------------------------------
// Page turning
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
int readingPercent(Context* ctx) {
    lv_obj_t* scroll = lv_obj_get_parent(ctx->contentWidget_);
    lv_obj_update_layout(scroll);
    const int32_t y = LV_MAX(lv_obj_get_scroll_y(scroll), 0);
    const int32_t maxY = y + LV_MAX(lv_obj_get_scroll_bottom(scroll), 0);
    // A chapter that fits on the screen has been seen completely
    const float fraction = maxY > 0 ? static_cast<float>(y) / maxY : 1.0f;
    if (!ctx->epub_) return static_cast<int>(fraction * 100);

    // Chapters differ a lot in length, so they count by their size
    const size_t index = static_cast<size_t>(LV_MAX(ctx->currentSpineIndex_, 0));
    const uint64_t before = ctx->epub_->getSizeBefore(index);
    const uint64_t current = ctx->epub_->getSizeBefore(index + 1) - before;
    const uint64_t total = ctx->epub_->getSizeBefore(ctx->epub_->getSpine().size());
    if (total == 0) return 0;
    return static_cast<int>((before + static_cast<uint64_t>(current * fraction)) * 100 / total);
}

void updateStatus(Context* ctx, int percent) {
    if (!ctx->statusLabel_ || !ctx->contentWidget_) return;
    lv_obj_t* scroll = lv_obj_get_parent(ctx->contentWidget_);
    const int32_t step = snapStep(lv_obj_get_height(scroll));
    if (step <= 0) return;
    const int32_t y = LV_MAX(lv_obj_get_scroll_y(scroll), 0);
    const int32_t maxY = y + LV_MAX(lv_obj_get_scroll_bottom(scroll), 0);
    const int pages = static_cast<int>((maxY + step - 1) / step) + 1;
    const int page = static_cast<int>((y + step - 1) / step) + 1;
    lv_label_set_text_fmt(ctx->statusLabel_, "Page %d of %d  -  %d%%", page, pages, percent);
}

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
