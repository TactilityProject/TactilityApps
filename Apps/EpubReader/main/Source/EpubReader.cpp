#include "EpubReader.h"
#include "HtmlStrip.h"              // stripHtmlToText

#include <app/paths.h>
#include <tactility/log.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char* TAG = "EpubReader";

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "tactility.epubreader";

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
// Recent books - one line per book, most recent first:
// path <tab> chapter <tab> offset <tab> percent <tab> title
// ---------------------------------------------------------------------------

static bool getUserFile(const char* name, char* path, size_t size) {
    return app_paths_get_user_data_path(APP_ID, name, path, size) == ERROR_NONE;
}

static void readRecents(Context* ctx) {
    char path[128];
    if (!getUserFile("recent.txt", path, sizeof(path))) return;
    FILE* f = fopen(path, "r");
    // A save may have been interrupted before the new file got its name
    if (!f) f = fopen((std::string(path) + ".tmp").c_str(), "r");
    if (!f) return;
    char line[512];
    while (ctx->recents_.size() < MAX_RECENT_BOOKS && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char* fields[5] = { line };
        int count = 1;
        for (char* c = line; *c != '\0' && count < 5; c++) {
            if (*c == '\t') {
                *c = '\0';
                fields[count++] = c + 1;
            }
        }
        if (count < 5 || fields[0][0] == '\0') continue;
        ctx->recents_.push_back({ fields[0], fields[4], atoi(fields[1]), atoi(fields[2]), atoi(fields[3]) });
    }
    fclose(f);
}

/** The single book that earlier versions of the app remembered */
static void readOldProgress(Context* ctx) {
    char path[128];
    if (!getUserFile("progress.txt", path, sizeof(path))) return;
    FILE* f = fopen(path, "r");
    if (!f) return;
    char filePath[256] = {};
    if (fgets(filePath, sizeof(filePath), f) != nullptr) {
        filePath[strcspn(filePath, "\r\n")] = '\0';
        int position = 0;
        if (fscanf(f, "%d", &position) != 1) position = 0;
        if (filePath[0] != '\0') {
            const std::string book = filePath;
            const bool isText = isTextFile(book);
            ctx->recents_.push_back({ book, book.substr(book.find_last_of('/') + 1),
                isText ? 0 : position, isText ? position : 0, 0 });
        }
    }
    fclose(f);
}

void writeRecents(Context* ctx) {
    char path[128];
    if (!getUserFile("recent.txt", path, sizeof(path))) return;
    // The previous list is only replaced when the new one is complete
    const std::string temporary = std::string(path) + ".tmp";
    FILE* f = fopen(temporary.c_str(), "w");
    if (!f) return;
    bool success = true;
    for (const auto& book : ctx->recents_) {
        success = fprintf(f, "%s\t%d\t%d\t%d\t%s\n", book.path.c_str(), book.chapter, book.offset, book.percent, book.title.c_str()) > 0 && success;
    }
    success = (fclose(f) == 0) && success;
    if (!success) {
        LOG_E(TAG, "Failed to save the recent books");
        unlink(temporary.c_str());
        return;
    }
    // FAT can't rename onto an existing file
    unlink(path);
    rename(temporary.c_str(), path);
}

void saveProgress(Context* ctx) {
    if (ctx->currentFilePath_.empty()) return;

    RecentBook book;
    book.path = ctx->currentFilePath_;
    book.title = book.path.substr(book.path.find_last_of('/') + 1);
    if (ctx->epub_) {
        std::string title = ctx->epub_->getMetadata("title");
        std::replace(title.begin(), title.end(), '\t', ' ');
        std::replace(title.begin(), title.end(), '\n', ' ');
        if (!title.empty()) book.title = title.substr(0, 128);
        book.chapter = ctx->currentSpineIndex_;
    }
    book.offset = static_cast<int>(ctx->pageOffset_);
    if (ctx->contentWidget_ != nullptr) {
        book.percent = readingPercent(ctx);
        updateStatus(ctx, book.percent);
    }

    auto& recents = ctx->recents_;
    for (size_t i = 0; i < recents.size(); i++) {
        if (recents[i].path == book.path) {
            recents.erase(recents.begin() + i);
            break;
        }
    }
    recents.insert(recents.begin(), book);
    if (recents.size() > MAX_RECENT_BOOKS) recents.resize(MAX_RECENT_BOOKS);
    writeRecents(ctx);
}

void epubReaderInit(Context* ctx) {
    // Ensure the app data directory exists
    char dir[128];
    app_paths_get_user_data_directory(APP_ID, dir, sizeof(dir));
    for (char* p = dir + 1; *p; ++p) {
        if (*p == '/') { *p = '\0'; mkdir(dir, 0755); *p = '/'; }
    }
    mkdir(dir, 0755);

    readRecents(ctx);
    if (ctx->recents_.empty()) readOldProgress(ctx);
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


void epubReaderTeardown(Context* ctx) {
    ++ctx->openToken_;          // invalidate any in-flight background open task
    unloadFonts();
    ctx->epub_ = nullptr;
    ctx->pageContent_ = {};     // release chapter/text memory
}
