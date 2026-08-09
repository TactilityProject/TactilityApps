#include "EpubReader.h"
#include "HtmlStrip.h"              // stripHtmlToText

#include <app/manager.h>
#include <app/paths.h>
#include <lvgl/widgets/toolbar.h>
#include <tactility/log.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* TAG = "EpubReader";

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "one.tactility.epubreader";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static std::string getAppDataRoot() {
    char path[128] = {0};
    app_paths_get_user_data_directory(APP_ID, path, sizeof(path));
    // path = /sdcard/tactility/app/one.tactility.epubreader → extract "/sdcard"
    // path = /data/tactility/app/one.tactility.epubreader → extract "/data"
    std::string s(path);
    size_t pos = s.find('/', 1);
    return (pos != std::string::npos) ? s.substr(0, pos) : "/sdcard";
}

// ---------------------------------------------------------------------------
// Books folder persistence
// ---------------------------------------------------------------------------

void loadBooksPath(Context* ctx) {
    char path[128];
    if (app_paths_get_user_data_path(APP_ID, "books_folder.txt", path, sizeof(path)) != ERROR_NONE) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char buf[256] = {};
    if (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        if (buf[0] != '\0') ctx->booksPath_ = buf;
    }
    fclose(f);
}

void saveBooksPath(Context* ctx) {
    char path[128];
    if (app_paths_get_user_data_path(APP_ID, "books_folder.txt", path, sizeof(path)) != ERROR_NONE) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", ctx->booksPath_.c_str());
    fclose(f);
}

// ---------------------------------------------------------------------------
// File type helpers (shared across translation units)
// ---------------------------------------------------------------------------

bool isSupportedFile(const std::string& name) {
    auto pos = name.rfind('.');
    if (pos == std::string::npos) return false;
    std::string ext = name.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".epub" || ext == ".txt";
}

bool isTextFile(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return false;
    std::string ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".txt";
}

// ---------------------------------------------------------------------------
// Persistence - plain text file in the app user-data directory
// ---------------------------------------------------------------------------

void saveProgress(Context* ctx) {
    if (ctx->currentFilePath_.empty()) return;
    char path[128];
    if (app_paths_get_user_data_path(APP_ID, "progress.txt", path, sizeof(path)) != ERROR_NONE) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    // Text mode: pageOffset_ is a scroll-Y pixel position; epub: spine chapter index.
    // The mode tag makes the file self-describing so the value's semantics are unambiguous.
    int savedIndex = ctx->textMode_ ? (int)ctx->pageOffset_ : ctx->currentSpineIndex_;
    fprintf(f, "%s\n%d\n%s\n", ctx->currentFilePath_.c_str(), savedIndex,
            ctx->textMode_ ? "text" : "epub");
    fclose(f);
}

bool loadProgress(std::string& outPath, int& outChapter, bool& outIsText) {
    char path[128];
    if (app_paths_get_user_data_path(APP_ID, "progress.txt", path, sizeof(path)) != ERROR_NONE) return false;
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char filePath[256] = {};
    bool ok = (fgets(filePath, sizeof(filePath), f) != nullptr);
    int chapter = 0;
    char modeStr[8] = "epub";  // default for backward-compat with old progress files
    if (ok) {
        size_t len = strlen(filePath);
        if (len > 0 && filePath[len - 1] == '\n') filePath[len - 1] = '\0';
        if (fscanf(f, "%d", &chapter) != 1) chapter = 0;
        fscanf(f, " %7s", modeStr);  // optional - old files won't have it
    }
    fclose(f);
    if (!ok || filePath[0] == '\0') return false;
    outPath    = filePath;
    outChapter = chapter;
    outIsText  = (strcmp(modeStr, "text") == 0);
    return true;
}

// ---------------------------------------------------------------------------
// Chapter loading and pagination
// ---------------------------------------------------------------------------

// Render all of pageContent_ as per-paragraph labels, then restore saved scroll position.
// Prev/Next navigate by scrolling one viewport height within the chapter; at chapter
// boundaries they load the adjacent chapter (like text mode, but across chapters).
void renderPage(Context* ctx) {
    if (!ctx->contentWidget_) return;
    lv_obj_clean(ctx->contentWidget_);
    if (ctx->pageContent_.empty()) {
        lv_obj_t* lbl = lv_label_create(ctx->contentWidget_);
        lv_label_set_text(lbl, "(Empty chapter)");
        lv_obj_scroll_to_y(lv_obj_get_parent(ctx->contentWidget_), 0, LV_ANIM_OFF);
        return;
    }
    renderSlice(ctx, ctx->pageContent_);
    lv_coord_t scrollY = (ctx->pageOffset_ > (size_t)LV_COORD_MAX) ? LV_COORD_MAX : (lv_coord_t)ctx->pageOffset_;
    lv_obj_scroll_to_y(lv_obj_get_parent(ctx->contentWidget_), scrollY, LV_ANIM_OFF);
}

void loadChapter(Context* ctx, int index, int direction) {
    if (!ctx->epub_ || !ctx->contentWidget_) return;

    const auto& spine = ctx->epub_->getSpine();

    // Auto-skip chapters whose HTML strips to nothing (image-only, boilerplate, etc.)
    // currentSpineIndex_ is NOT updated until we confirm the chapter has content -
    // if we exhaust the spine before finding any, we return with it unchanged so
    // subsequent navigation calls aren't confused by a corrupted index.
    while (true) {
        if (index < 0 || index >= (int)spine.size()) return;

        std::string html = ctx->epub_->readFile(spine[index].href, MAX_CHAPTER_HTML);
        stripHtmlToText(html, ctx->pageContent_);
        html = {};  // release raw HTML before rendering

        if (!ctx->pageContent_.empty()) break;

        // Chapter stripped to nothing - advance in the navigation direction
        if (direction == 0) {
            ctx->currentSpineIndex_ = index;
            ctx->pageOffset_        = 0;
            LOG_W(TAG, "Chapter %d stripped to nothing and no direction to skip - blank chapter", index);
            lv_obj_clean(ctx->contentWidget_);
            lv_obj_t* lbl = lv_label_create(ctx->contentWidget_);
            lv_label_set_text(lbl, "(Chapter content unavailable)");
            return;
        }
        index += direction;
    }

    ctx->currentSpineIndex_ = index;
    ctx->pageOffset_        = 0;
    renderPage(ctx);
    // When arriving from a later chapter (going backward), jump to the end of this chapter
    // after LVGL has laid out the labels (deferred so content height is known).
    if (direction < 0) lv_async_call(asyncScrollToEnd, ctx);
    saveProgress(ctx);
}

void openTocDialog(Context* ctx) {
    if (!ctx->epub_) return;
    const auto& toc = ctx->epub_->getToc();
    if (toc.empty()) return;

    std::vector<const char*> argv;
    argv.reserve(toc.size() + 1);
    argv.push_back("Table of Contents");
    for (const auto& item : toc) {
        argv.push_back(item.title.c_str());
    }
    app_manager_start_for_result(
        "SelectionDialog", ctx->appInstanceId,
        (int)argv.size(), argv.data(), &ctx->tocDialogId_
    );
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

void epubReaderCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    // Hard requirement: without PSRAM this app cannot parse EPUBs or hold fonts.
    // Show a one-shot alert then stop; psramAlertId_ prevents re-launching the dialog
    // if this is called again (e.g. after a dialog roundtrip) before the app has closed.
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        if (ctx->psramAlertId_ == 0) {
            const char* argv[] = {
                "PSRAM Required",
                "Epub Reader requires a device with PSRAM and cannot run on this hardware.",
                "OK",
            };
            app_manager_start_for_result(
                "AlertDialog", ctx->appInstanceId, 3, argv, &ctx->psramAlertId_
            );
        }
        return;
    }

    loadFonts(ctx);

    if (ctx->dataRoot_.empty()) {
        ctx->dataRoot_ = getAppDataRoot();
        // Ensure the app data directory exists (needed for progress.txt and books_folder.txt)
        char dir[128];
        app_paths_get_user_data_directory(APP_ID, dir, sizeof(dir));
        for (char* p = dir + 1; *p; ++p) {
            if (*p == '/') { *p = '\0'; mkdir(dir, 0755); *p = '/'; }
        }
        mkdir(dir, 0755);
        // Load saved books folder; start browser there if set, otherwise dataRoot_
        loadBooksPath(ctx);
        ctx->browsePath_ = ctx->booksPath_.empty() ? ctx->dataRoot_ : ctx->booksPath_;
    }

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    ctx->toolbar_ = lvgl_toolbar_create(parent, "Epub Reader");

    ctx->wrapperWidget_ = lv_obj_create(parent);
    lv_obj_set_width(ctx->wrapperWidget_, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->wrapperWidget_, 1);
    lv_obj_set_flex_flow(ctx->wrapperWidget_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctx->wrapperWidget_, 0, 0);
    lv_obj_set_style_border_width(ctx->wrapperWidget_, 0, 0);
    lv_obj_set_style_bg_opa(ctx->wrapperWidget_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(ctx->wrapperWidget_, LV_OBJ_FLAG_SCROLLABLE);

    // Attempt to open a book: launch parameter → saved progress (first show only).
    // Both paths use lv_async_call so EpubService::open runs at fresh stack
    // depth (not nested inside the deep window_manager→createWidgets call chain).
    bool asyncOpen = false;
    if (!ctx->epub_) {
        if (!ctx->launchFilePath_.empty()) {
            LOG_I(TAG, "Opening from parameter: %s", ctx->launchFilePath_.c_str());
            ctx->pendingFilePath_ = ctx->launchFilePath_;
            lv_async_call(asyncOpenEpub, ctx);
            asyncOpen = true;
        }
    }
    bool asyncRestore = false;
    if (!ctx->epub_ && !asyncOpen) {
        std::string savedPath;
        int savedChapter = 0;
        bool savedIsText = false;
        if (loadProgress(savedPath, savedChapter, savedIsText)) {
            // Schedule open via lv_async_call so it runs at the same call-stack
            // depth as asyncOpenEpub - calling EpubService::open directly here
            // adds extra frames that overflow the task stack.
            ctx->pendingFilePath_   = savedPath;
            ctx->currentSpineIndex_ = savedChapter;
            lv_async_call(asyncRestoreEpub, ctx);
            asyncRestore = true;
            LOG_I(TAG, "Scheduling restore: %s ch%d", savedPath.c_str(), savedChapter);
        }
    }

    if (ctx->epub_ && ctx->epub_->isValid()) {
        setReaderToolbarButtons(ctx);
        buildReaderUI(ctx, ctx->wrapperWidget_);
    } else if (!asyncRestore && !asyncOpen) {
        ctx->epub_ = nullptr;
        setBrowserToolbarButtons(ctx);
        buildBrowserUI(ctx, ctx->wrapperWidget_);
    } else {
        // asyncOpenEpub / asyncRestoreEpub will replace the wrapper contents when
        // they fire; show an empty placeholder for now (no browser flash on ePaper)
    }
}

void epubReaderTeardown(Context* ctx) {
    ++ctx->openToken_;          // invalidate any in-flight background open task
    unloadFonts();
    ctx->contentWidget_  = nullptr;
    ctx->wrapperWidget_ = nullptr;
    ctx->toolbar_       = nullptr;
    ctx->pageContent_   = {};   // release chapter/text memory
    ctx->pageOffset_    = 0;
    ctx->textMode_      = false;
}
