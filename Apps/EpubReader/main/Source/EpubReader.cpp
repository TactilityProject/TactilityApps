#include "EpubReader.h"
#include "HtmlStrip.h"              // stripHtmlToText
#include <tt_bundle.h>
#include <tt_lvgl_toolbar.h>
#include <tt_app_alertdialog.h>
#include <tt_app_selectiondialog.h>
#include <tactility/log.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* TAG = "EpubReader";
static const char* EPUB_FILE_ARGUMENT = "file";

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::string EpubReader::getAppDataRoot(AppHandle app) {
    char path[128] = {0};
    size_t sz = sizeof(path);
    tt_app_get_user_data_path(app, path, &sz);
    // path = /sdcard/user/app/one.tactility.epubreader → extract "/sdcard"
    // path = /data/user/app/one.tactility.epubreader → extract "/data"
    std::string s(path);
    size_t pos = s.find('/', 1);
    return (pos != std::string::npos) ? s.substr(0, pos) : "/sdcard";
}

// ---------------------------------------------------------------------------
// Books folder persistence
// ---------------------------------------------------------------------------

void EpubReader::loadBooksPath() {
    if (!appHandle_) return;
    char path[128]; size_t sz = sizeof(path);
    tt_app_get_user_data_child_path(appHandle_, "books_folder.txt", path, &sz);
    FILE* f = fopen(path, "r");
    if (!f) return;
    char buf[256] = {};
    if (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        if (buf[0] != '\0') booksPath_ = buf;
    }
    fclose(f);
}

void EpubReader::saveBooksPath() {
    if (!appHandle_) return;
    char path[128]; size_t sz = sizeof(path);
    tt_app_get_user_data_child_path(appHandle_, "books_folder.txt", path, &sz);
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s\n", booksPath_.c_str());
    fclose(f);
}

// ---------------------------------------------------------------------------
// File type helpers (static members - used by UI and Async TUs via the class)
// ---------------------------------------------------------------------------

bool EpubReader::isSupportedFile(const std::string& name) {
    auto pos = name.rfind('.');
    if (pos == std::string::npos) return false;
    std::string ext = name.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".epub" || ext == ".txt";
}

bool EpubReader::isTextFile(const std::string& path) {
    auto pos = path.rfind('.');
    if (pos == std::string::npos) return false;
    std::string ext = path.substr(pos);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    return ext == ".txt";
}

// ---------------------------------------------------------------------------
// Persistence - plain text file in the app user-data directory
// ---------------------------------------------------------------------------

void EpubReader::saveProgress() {
    if (currentFilePath_.empty() || !appHandle_) return;
    char path[128]; size_t sz = sizeof(path);
    tt_app_get_user_data_child_path(appHandle_, "progress.txt", path, &sz);
    FILE* f = fopen(path, "w");
    if (!f) return;
    // Text mode: pageOffset_ is a scroll-Y pixel position; epub: spine chapter index.
    // The mode tag makes the file self-describing so the value's semantics are unambiguous.
    int savedIndex = textMode_ ? (int)pageOffset_ : currentSpineIndex_;
    fprintf(f, "%s\n%d\n%s\n", currentFilePath_.c_str(), savedIndex,
            textMode_ ? "text" : "epub");
    fclose(f);
}

bool EpubReader::loadProgress(std::string& outPath, int& outChapter, bool& outIsText) {
    if (!appHandle_) return false;
    char path[128]; size_t sz = sizeof(path);
    tt_app_get_user_data_child_path(appHandle_, "progress.txt", path, &sz);
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
void EpubReader::renderPage() {
    if (!contentWidget_) return;
    lv_obj_clean(contentWidget_);
    if (pageContent_.empty()) {
        lv_obj_t* lbl = lv_label_create(contentWidget_);
        lv_label_set_text(lbl, "(Empty chapter)");
        lv_obj_scroll_to_y(lv_obj_get_parent(contentWidget_), 0, LV_ANIM_OFF);
        return;
    }
    renderSlice(pageContent_);
    lv_coord_t scrollY = (pageOffset_ > (size_t)LV_COORD_MAX) ? LV_COORD_MAX : (lv_coord_t)pageOffset_;
    lv_obj_scroll_to_y(lv_obj_get_parent(contentWidget_), scrollY, LV_ANIM_OFF);
}

void EpubReader::loadChapter(int index, int direction) {
    if (!epub_ || !contentWidget_) return;

    const auto& spine = epub_->getSpine();

    // Auto-skip chapters whose HTML strips to nothing (image-only, boilerplate, etc.)
    // currentSpineIndex_ is NOT updated until we confirm the chapter has content -
    // if we exhaust the spine before finding any, we return with it unchanged so
    // subsequent navigation calls aren't confused by a corrupted index.
    while (true) {
        if (index < 0 || index >= (int)spine.size()) return;

        std::string html = epub_->readFile(spine[index].href, MAX_CHAPTER_HTML);
        stripHtmlToText(html, pageContent_);
        html = {};  // release raw HTML before rendering

        if (!pageContent_.empty()) break;

        // Chapter stripped to nothing - advance in the navigation direction
        if (direction == 0) {
            currentSpineIndex_ = index;
            pageOffset_        = 0;
            LOG_W(TAG, "Chapter %d stripped to nothing and no direction to skip - blank chapter", index);
            lv_obj_clean(contentWidget_);
            lv_obj_t* lbl = lv_label_create(contentWidget_);
            lv_label_set_text(lbl, "(Chapter content unavailable)");
            return;
        }
        index += direction;
    }

    currentSpineIndex_ = index;
    pageOffset_        = 0;
    renderPage();
    // When arriving from a later chapter (going backward), jump to the end of this chapter
    // after LVGL has laid out the labels (deferred so content height is known).
    if (direction < 0) lv_async_call(asyncScrollToEnd, this);
    saveProgress();
}

void EpubReader::openTocDialog() {
    if (!epub_) return;
    const auto& toc = epub_->getToc();
    if (toc.empty()) return;

    std::vector<const char*> titles;
    titles.reserve(toc.size());
    for (const auto& item : toc) {
        titles.push_back(item.title.c_str());
    }
    tocDialogId_ = tt_app_selectiondialog_start(
        "Table of Contents", (int)titles.size(), titles.data()
    );
}

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

void EpubReader::onShow(AppHandle app, lv_obj_t* parent) {
    appHandle_ = app;

    // Hard requirement: without PSRAM this app cannot parse EPUBs or hold fonts.
    // Show a one-shot alert then stop; psramAlertId_ prevents re-launching the dialog
    // if onShow is called again before tt_app_stop() takes effect.
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        if (psramAlertId_ == 0) {
            static const char* kButtons[] = { "OK" };
            psramAlertId_ = tt_app_alertdialog_start(
                "PSRAM Required",
                "Epub Reader requires a device with PSRAM and cannot run on this hardware.",
                kButtons, 1
            );
        }
        return;
    }

    loadFonts();

    if (dataRoot_.empty()) {
        dataRoot_ = getAppDataRoot(app);
        // Ensure the app data directory exists (needed for progress.txt and books_folder.txt)
        char dir[128]; size_t sz = sizeof(dir);
        tt_app_get_user_data_path(app, dir, &sz);
        for (char* p = dir + 1; *p; ++p) {
            if (*p == '/') { *p = '\0'; mkdir(dir, 0755); *p = '/'; }
        }
        mkdir(dir, 0755);
        // Load saved books folder; start browser there if set, otherwise dataRoot_
        loadBooksPath();
        browsePath_ = booksPath_.empty() ? dataRoot_ : booksPath_;
    }

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    toolbar_ = tt_lvgl_toolbar_create_for_app(parent, app);

    wrapperWidget_ = lv_obj_create(parent);
    lv_obj_set_width(wrapperWidget_, LV_PCT(100));
    lv_obj_set_flex_grow(wrapperWidget_, 1);
    lv_obj_set_flex_flow(wrapperWidget_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(wrapperWidget_, 0, 0);
    lv_obj_set_style_border_width(wrapperWidget_, 0, 0);
    lv_obj_set_style_bg_opa(wrapperWidget_, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(wrapperWidget_, LV_OBJ_FLAG_SCROLLABLE);

    // Apply chapter jump from TOC dialog (stored in onResult)
    if (pendingChapterIndex_ >= 0) {
        currentSpineIndex_ = pendingChapterIndex_;
        pendingChapterIndex_ = -1;
    }

    // Attempt to open a book: launch parameter → saved progress (first show only).
    // Both paths use lv_async_call so EpubService::open runs at fresh stack
    // depth (not nested inside the deep GuiService→onShow call chain).
    bool asyncOpen = false;
    if (!epub_) {
        BundleHandle bundle = tt_app_get_parameters(app);
        if (bundle) {
            char filepath[256] = {0};
            if (tt_bundle_opt_string(bundle, EPUB_FILE_ARGUMENT, filepath, (uint32_t)sizeof(filepath))) {
                LOG_I(TAG, "Opening from parameter: %s", filepath);
                pendingFilePath_ = filepath;
                lv_async_call(asyncOpenEpub, this);
                asyncOpen = true;
            }
        }
    }
    bool asyncRestore = false;
    if (!epub_ && !asyncOpen) {
        std::string savedPath;
        int savedChapter = 0;
        bool savedIsText = false;
        if (loadProgress(savedPath, savedChapter, savedIsText)) {
            // Schedule open via lv_async_call so it runs at the same call-stack
            // depth as asyncOpenEpub - calling EpubService::open directly here
            // (from GuiService→onShow) adds extra frames that overflow the task stack.
            pendingFilePath_   = savedPath;
            currentSpineIndex_ = savedChapter;
            lv_async_call(asyncRestoreEpub, this);
            asyncRestore = true;
            LOG_I(TAG, "Scheduling restore: %s ch%d", savedPath.c_str(), savedChapter);
        }
    }

    if (epub_ && epub_->isValid()) {
        setReaderToolbarButtons();
        buildReaderUI(wrapperWidget_);
    } else if (!asyncRestore && !asyncOpen) {
        epub_ = nullptr;
        setBrowserToolbarButtons();
        buildBrowserUI(wrapperWidget_);
    } else {
        // asyncOpenEpub / asyncRestoreEpub will replace the wrapper contents when
        // they fire; show an empty placeholder for now (no browser flash on ePaper)
    }
}

void EpubReader::onHide(AppHandle /*app*/) {
    ++openToken_;          // invalidate any in-flight background open task
    // Skip font unload during a dialog roundtrip (tocDialogId_ is non-zero while
    // the TOC selection dialog is open). loadFonts() guards against double-loading,
    // so fonts will be reused as-is when onShow fires after the dialog closes.
    // When truly exiting, no dialog is open and fonts are freed as normal.
    if (tocDialogId_ == 0 && psramAlertId_ == 0) {
        unloadFonts();
    }
    contentWidget_  = nullptr;
    wrapperWidget_ = nullptr;
    toolbar_       = nullptr;
    appHandle_     = nullptr;
    pageContent_   = {};   // release chapter/text memory
    pageOffset_    = 0;
    textMode_      = false;
}

void EpubReader::onResult(AppHandle /*app*/, void* /*data*/, AppLaunchId launchId,
                           AppResult /*result*/, BundleHandle resultData) {
    // Never touch LVGL objects here - store state for onShow
    if (launchId == psramAlertId_ && psramAlertId_ != 0) {
        tt_app_stop();
        return;
    }
    if (launchId == tocDialogId_ && tocDialogId_ != 0) {
        tocDialogId_ = 0;
        if (resultData) {
            int32_t selection = tt_app_selectiondialog_get_result_index(resultData);
            if (selection >= 0 && epub_) {
                const auto& toc   = epub_->getToc();
                const auto& spine = epub_->getSpine();
                if (selection < (int32_t)toc.size()) {
                    pendingChapterIndex_ = -1;  // Reset before searching
                    for (size_t i = 0; i < spine.size(); ++i) {
                        if (spine[i].href == toc[selection].href) {
                            pendingChapterIndex_ = (int)i;
                            break;
                        }
                    }
                }
            }
        }
    }
}
