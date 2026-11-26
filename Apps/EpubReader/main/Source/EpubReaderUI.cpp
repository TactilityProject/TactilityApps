#include "EpubReader.h"
#include <app/event.h>
#include <app/paths.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/list.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/dialog.h>
#include <tactility/device.h>
#include <tactility/drivers/keyboard.h>
#include <tactility/drivers/trackball.h>
#include <tactility/log.h>
#include <esp_heap_caps.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* TAG = "EpubReader";

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "tactility.epubreader";

// Runtime-loaded Noto Serif fonts - 4 variants for the active display size tier.
// Loaded by loadFonts() on createWidgets, freed by unloadFonts() in epubReaderTeardown().
// lv_binfont_create/destroy are exported by the firmware's lvgl-module symbols.
// NOTE: These are file-scope statics intentionally. Tactility runs one instance of
// each app at a time, so there is no multi-instance aliasing or use-after-free risk.
static lv_font_t* s_fontRegular    = nullptr;
static lv_font_t* s_fontItalic     = nullptr;
static lv_font_t* s_fontBold       = nullptr;
static lv_font_t* s_fontBoldItalic = nullptr;

const lv_font_t* selectContentFont(bool italic, bool bold) {
    const lv_font_t* f = bold ? (italic ? s_fontBoldItalic : s_fontBold)
                               : (italic ? s_fontItalic     : s_fontRegular);
    if (f) return f;
    // Variant not loaded (e.g. no-PSRAM device only loads regular) - fall back gracefully.
    return s_fontRegular ? s_fontRegular : lv_font_get_default();
}

// ---------------------------------------------------------------------------
// Font lifecycle
// ---------------------------------------------------------------------------

void loadFonts(Context* ctx) {
    if (s_fontRegular) return;  // already loaded

    // Without PSRAM the heap is too constrained to hold even a single binary font
    // alongside the epub parser and LVGL allocations - skip loading entirely.
    // epubReaderCreateWidgets() will have already shown an alert dialog for this case.
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) return;

    // Verify the assets directory exists before attempting individual file loads.
    // This produces one clear diagnostic instead of one error per font file.
    char assetsDir[256];
    if (app_paths_get_assets_directory(APP_ID, assetsDir, sizeof(assetsDir)) != ERROR_NONE) {
        LOG_E(TAG, "loadFonts: could not resolve assets path");
        return;
    }
    struct stat st;
    if (stat(assetsDir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        LOG_W(TAG, "loadFonts: assets directory not found: %s", assetsDir);
        return;
    }

    lv_coord_t w = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t h = lv_display_get_vertical_resolution(nullptr);
    const char* sz;
    if      (w >= 1000 || h >= 1000) sz = "36";
    else if (w >= 600  || h >= 480 ) sz = "28";
    else if (w >= 400  || h >= 300 ) sz = "20";
    else                              sz = "16";

    struct Variant { const char* suffix; lv_font_t** ptr; };
    Variant variants[] = {
        { "regular",     &s_fontRegular    },
        { "italic",      &s_fontItalic     },
        { "bold",        &s_fontBold       },
        { "bold_italic", &s_fontBoldItalic },
    };
    for (auto& v : variants) {
        char filename[64];
        snprintf(filename, sizeof(filename), "font_%spt_%s.bin", sz, v.suffix);
        char assetPath[256];
        if (app_paths_get_assets_path(APP_ID, filename, assetPath, sizeof(assetPath)) != ERROR_NONE) {
            LOG_E(TAG, "loadFonts: no asset path for %s", filename);
            continue;
        }
        // LVGL STDIO driver letter 'A' with empty path prefix: "A:/foo" → fopen("/foo")
        std::string lvglPath = std::string("A:") + assetPath;
        *v.ptr = lv_binfont_create(lvglPath.c_str());
        if (!*v.ptr) LOG_E(TAG, "lv_binfont_create failed: %s", assetPath);
        else         LOG_I(TAG, "Font loaded: %s (%dpt)", v.suffix, atoi(sz));
    }

}

void unloadFonts() {
    lv_font_t** ptrs[] = { &s_fontRegular, &s_fontItalic, &s_fontBold, &s_fontBoldItalic };
    for (auto* p : ptrs) {
        if (*p) { lv_binfont_destroy(*p); *p = nullptr; }
    }
}

// ---------------------------------------------------------------------------
// renderSlice helpers - inline bold/italic via lv_spangroup
// ---------------------------------------------------------------------------

// Returns true if the text segment contains any inline bold/italic ESC tokens.
static bool hasInlineTokens(const char* s, size_t len) {
    for (size_t i = 0; i + 1 < len; ++i) {
        if ((unsigned char)s[i] == 0x1B) {
            char t = s[i + 1];
            if (t == 'B' || t == 'b' || t == 'I' || t == 'i') return true;
        }
    }
    return false;
}

// Builds an lv_spangroup for a paragraph containing inline bold/italic ESC tokens.
// Each ESC+B/b/I/i boundary becomes a new span with the appropriate font.
static void renderSpanParagraph(lv_obj_t* parent, const char* seg, size_t len,
                                lv_text_align_t align) {
    lv_obj_t* sg = lv_spangroup_create(parent);
    lv_obj_set_width(sg, LV_PCT(100));
    lv_obj_set_height(sg, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(sg, 0, 0);  // keep heights = N*lineH (same rule as lv_label)
    lv_spangroup_set_align(sg, align);
    lv_spangroup_set_mode(sg, LV_SPAN_MODE_BREAK);  // wrap to content height
    lv_obj_set_clickable(sg, false);

    bool bold = false, italic = false;
    std::string run;
    run.reserve(len);

    auto flushRun = [&]() {
        if (run.empty()) return;
        lv_span_t* span = lv_spangroup_add_span(sg);
        lv_span_set_text(span, run.c_str());
        lv_style_set_text_font(lv_span_get_style(span), selectContentFont(italic, bold));
        run.clear();
    };

    for (size_t i = 0; i < len; ++i) {
        unsigned char c = (unsigned char)seg[i];
        if (c == 0x1B && i + 1 < len) {
            char tok = seg[i + 1];
            if (tok == 'B') { flushRun(); bold   = true;  ++i; continue; }
            if (tok == 'b') { flushRun(); bold   = false; ++i; continue; }
            if (tok == 'I') { flushRun(); italic = true;  ++i; continue; }
            if (tok == 'i') { flushRun(); italic = false; ++i; continue; }
        }
        run += (char)c;
    }
    flushRun();
    lv_spangroup_refresh(sg);
}

// ---------------------------------------------------------------------------
// renderSlice - parse ESC-encoded page slice into per-paragraph LVGL widgets
// ---------------------------------------------------------------------------
// ESC token protocol (see HtmlStrip.h):
//   Every paragraph starts with ESC + 'L'|'C'|'R' (alignment).
//   Paragraphs are separated by '\n\n'.
//   Single '\n' = line break within a paragraph.
//   Inline ESC+'B'/'b'/'I'/'i' tokens delimit bold/italic runs.
//
// Paragraphs without inline tokens → lv_label (lighter, fewer allocations).
// Paragraphs with inline tokens    → lv_spangroup (per-span font selection).
void renderSlice(Context* ctx, const std::string& slice) {
    if (!ctx->contentWidget_) return;

    const lv_font_t* font = selectContentFont(false, false);
    size_t pos = 0;
    const size_t len = slice.size();

    while (pos < len) {
        // Find the next paragraph boundary (\n\n) or end of string
        size_t nlnl    = slice.find("\n\n", pos);
        size_t paraEnd = (nlnl != std::string::npos) ? nlnl : len;

        // Extract alignment from leading ESC token
        lv_text_align_t align = LV_TEXT_ALIGN_LEFT;
        size_t textStart = pos;
        if (paraEnd > pos + 1 && (unsigned char)slice[pos] == 0x1B) {
            char code = slice[pos + 1];
            if      (code == 'C') align = LV_TEXT_ALIGN_CENTER;
            else if (code == 'R') align = LV_TEXT_ALIGN_RIGHT;
            textStart = pos + 2;
        }

        const char* seg = slice.data() + textStart;
        size_t segLen   = paraEnd - textStart;

        // Skip blank paragraphs
        bool hasContent = false;
        for (size_t k = 0; k < segLen; ++k) {
            char ch = seg[k];
            if (ch != ' ' && ch != '\n') { hasContent = true; break; }
        }

        if (hasContent) {
            if (hasInlineTokens(seg, segLen)) {
                // Mixed-style paragraph: spangroup renders inline bold/italic runs
                renderSpanParagraph(ctx->contentWidget_, seg, segLen, align);
            } else {
                // Plain paragraph: lv_label (simpler, lower memory overhead)
                lv_obj_t* lbl = lv_label_create(ctx->contentWidget_);
                lv_obj_set_width(lbl, LV_PCT(100));
                lv_obj_set_style_pad_all(lbl, 0, 0);  // override theme padding; keeps heights = N*lineH
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
                lv_obj_set_style_text_font(lbl, font, 0);
                lv_obj_set_style_text_align(lbl, align, 0);
                lv_label_set_text(lbl, std::string(seg, segLen).c_str());
            }
        }

        pos = (nlnl != std::string::npos) ? nlnl + 2 : len;
    }
}

// ---------------------------------------------------------------------------
// Dialogs and file selection
// ---------------------------------------------------------------------------

static void onDialogResult(int32_t index, void* userData) {
    // Handled on the app task: loading a chapter needs more stack than the dialog task has
    auto* ctx = static_cast<Context*>(userData);
    ctx->dialogResult_ = index;
    task_event_group_signal(ctx->eventGroup_, ctx->dialogBit_);
}

static void showSelection(Context* ctx, ReaderDialog dialog, const char* title, const std::vector<const char*>& items) {
    ctx->dialog_ = dialog;
    if (window_manager_selection_dialog(ctx->appInstanceId, title, items.data(), (uint32_t)items.size(), onDialogResult, ctx) != ERROR_NONE) {
        ctx->dialog_ = ReaderDialog::None;
    }
}

static bool hasContents(Context* ctx) {
    return ctx->epub_ && !ctx->epub_->getToc().empty();
}

static void showContents(Context* ctx) {
    if (!hasContents(ctx)) return;
    std::vector<const char*> items;
    for (const auto& item : ctx->epub_->getToc()) {
        items.push_back(item.title.c_str());
    }
    showSelection(ctx, ReaderDialog::Contents, "Table of Contents", items);
}

static void showMenu(Context* ctx) {
    std::vector<const char*> items;
    ctx->menuActions_.clear();
    auto add = [&](MenuAction action, const char* label) {
        ctx->menuActions_.push_back(action);
        items.push_back(label);
    };
    if (hasContents(ctx)) add(MenuAction::Contents, "Contents");
    add(MenuAction::OpenFile, "Open file");
    add(MenuAction::CloseBook, "Close book");
    showSelection(ctx, ReaderDialog::Menu, "Book", items);
}

/** Runs on the dialog task */
static void onFileDialogResult(const char* const* paths, uint32_t pathCount, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->pickedPath_ = pathCount > 0 ? paths[0] : "";
    task_event_group_signal(ctx->eventGroup_, ctx->fileBit_);
}

void startFileSelection(Context* ctx) {
    static const char* const extensions[] = { "epub", "txt" };
    // Without a path, browsing starts at paths_get_root_path()
    const WindowFileDialogOptions options = {
        .flags = 0,
        .path = nullptr,
        .name = nullptr,
        .extensions = extensions,
        .extension_count = 2,
    };
    const error_t error = window_manager_file_dialog(ctx->appInstanceId, &options, onFileDialogResult, ctx);
    if (error != ERROR_NONE) {
        LOG_E(TAG, "Failed to show the file dialog: %d", error);
    }
}

void handleFileResult(Context* ctx) {
    const std::string path = ctx->pickedPath_;
    if (!path.empty()) {
        lvgl_lock();
        openBook(ctx, path);
        showView(ctx);
        lvgl_unlock();
    }
}

void handleDialogResult(Context* ctx) {
    const ReaderDialog dialog = ctx->dialog_.exchange(ReaderDialog::None);
    const int32_t index = ctx->dialogResult_;
    if (index < 0) return;

    if (dialog == ReaderDialog::Contents) {
        lvgl_lock();
        if (ctx->epub_ && index < (int32_t)ctx->epub_->getToc().size()) {
            const std::string href = ctx->epub_->getToc()[index].href;
            const auto& spine = ctx->epub_->getSpine();
            for (size_t i = 0; i < spine.size(); ++i) {
                if (spine[i].href == href) {
                    loadChapter(ctx, (int)i, 0);
                    break;
                }
            }
        }
        lvgl_unlock();
    } else if (dialog == ReaderDialog::Menu && index < (int32_t)ctx->menuActions_.size()) {
        switch (ctx->menuActions_[index]) {
            case MenuAction::Contents:
                lvgl_lock();
                showContents(ctx);
                lvgl_unlock();
                break;
            case MenuAction::OpenFile:
                startFileSelection(ctx);
                break;
            case MenuAction::CloseBook:
                lvgl_lock();
                closeBook(ctx);
                showView(ctx);
                lvgl_unlock();
                break;
        }
    }
}

// ---------------------------------------------------------------------------
// Widget events
// ---------------------------------------------------------------------------

void asyncShowView(void* data) {
    showView(static_cast<Context*>(data));
}

static void onExitPressed(lv_event_t* e) {
    app_event_emit_close(static_cast<Context*>(lv_event_get_user_data(e))->appInstanceId);
}

/** From a book to the library */
static void goBack(Context* ctx) {
    closeBook(ctx);
    // The widgets can't be rebuilt from their own event
    lv_async_call(asyncShowView, ctx);
}

static void onBackPressed(lv_event_t* e) {
    goBack(static_cast<Context*>(lv_event_get_user_data(e)));
}

static void onOpenFilePressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    task_event_group_signal(ctx->eventGroup_, ctx->openFileBit_);
}

static void onContentsPressed(lv_event_t* e) {
    showContents(static_cast<Context*>(lv_event_get_user_data(e)));
}

static void onMenuPressed(lv_event_t* e) {
    showMenu(static_cast<Context*>(lv_event_get_user_data(e)));
}

static void onRecentPressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    const auto index = reinterpret_cast<uintptr_t>(lv_obj_get_user_data(lv_event_get_current_target_obj(e)));
    if (index < ctx->recents_.size()) {
        openBook(ctx, ctx->recents_[index].path);
        lv_async_call(asyncShowView, ctx);
    }
}

/** Left half turns back, right half forward. A key press without a position opens the menu. */
static void onReaderTap(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    if (!indev) return;
    if (lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        showMenu(ctx);
        return;
    }
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    if (pt.x < lv_display_get_horizontal_resolution(nullptr) / 2) doPrev(ctx);
    else doNext(ctx);
}

static void onReaderScrolled(lv_event_t* e) {
    rememberPosition(static_cast<Context*>(lv_event_get_user_data(e)));
}

static void onReaderKey(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    switch (lv_event_get_key(e)) {
        case LV_KEY_LEFT:
        case LV_KEY_UP:
            doPrev(ctx);
            break;
        case LV_KEY_RIGHT:
        case LV_KEY_DOWN:
        case ' ':
            doNext(ctx);
            break;
        case LV_KEY_ESC:
            goBack(ctx);
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Views
// ---------------------------------------------------------------------------

static void buildLibraryUI(Context* ctx, lv_obj_t* parent) {
    if (!ctx->libraryError_.empty()) {
        lv_obj_t* error = lv_label_create(parent);
        lv_obj_set_width(error, LV_PCT(100));
        lv_obj_set_style_pad_all(error, 6, 0);
        lv_label_set_long_mode(error, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(error, ctx->libraryError_.c_str());
    }

    if (ctx->recents_.empty()) {
        lv_obj_t* empty = lv_label_create(parent);
        lv_obj_set_width(empty, LV_PCT(100));
        lv_obj_set_flex_grow(empty, 1);
        lv_obj_set_style_pad_all(empty, 8, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(empty, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(empty, "No books yet. Open an .epub or .txt file.");
    } else {
        lv_obj_t* list = lvgl_list_create(parent);
        lv_obj_set_width(list, LV_PCT(100));
        lv_obj_set_flex_grow(list, 1);
        for (size_t i = 0; i < ctx->recents_.size(); i++) {
            const RecentBook& book = ctx->recents_[i];
            lv_obj_t* button = lvgl_list_add_button(list, nullptr, nullptr);
            lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
            lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
            lv_obj_add_event_cb(button, onRecentPressed, LV_EVENT_SHORT_CLICKED, ctx);

            lv_obj_t* title = lv_label_create(button);
            lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_height(title, lv_font_get_line_height(lv_obj_get_style_text_font(title, LV_PART_MAIN)));
            lv_obj_set_width(title, 0);
            lv_obj_set_flex_grow(title, 1);
            lv_label_set_text(title, book.title.c_str());

            lv_obj_t* percent = lv_label_create(button);
            lv_label_set_text_fmt(percent, "%d%%", book.percent);
            lv_obj_set_style_text_opa(percent, LV_OPA_70, 0);
        }
        lvgl_list_hide_last_divider(list);
    }

    lv_obj_t* open = lv_button_create(parent);
    lv_obj_set_width(open, LV_PCT(100));
    lv_obj_add_event_cb(open, onOpenFilePressed, LV_EVENT_SHORT_CLICKED, ctx);
    lv_obj_t* label = lv_label_create(open);
    lv_label_set_text(label, "Open file");
    lv_obj_center(label);
}

static void buildReaderUI(Context* ctx, lv_obj_t* parent) {
    // Loading a chapter resets the position, so it's restored afterwards
    const size_t offset = ctx->pageOffset_;

    lv_obj_t* scroll = lv_obj_create(parent);
    lv_obj_set_width(scroll, LV_PCT(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_pad_all(scroll, 6, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);

    // LV_EVENT_CLICKED only fires on press+release with minimal movement,
    // so it won't conflict with vertical scrolling.
    lv_obj_add_event_cb(scroll, onReaderTap, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(scroll, onReaderKey, LV_EVENT_KEY, ctx);
    // Dragging scrolls too
    lv_obj_add_event_cb(scroll, onReaderScrolled, LV_EVENT_SCROLL_END, ctx);

    ctx->statusLabel_ = lv_label_create(parent);
    lv_obj_set_width(ctx->statusLabel_, LV_PCT(100));
    lv_obj_set_style_pad_hor(ctx->statusLabel_, 6, 0);
    lv_obj_set_style_text_align(ctx->statusLabel_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(ctx->statusLabel_, lvgl_get_text_font(FONT_SIZE_SMALL), 0);
    lv_obj_set_style_text_opa(ctx->statusLabel_, LV_OPA_70, 0);
    lv_label_set_text(ctx->statusLabel_, "");

    if (ctx->textMode_) {
        // Plain text file: single label, page turns scroll by screen height.
        ctx->contentWidget_ = lv_label_create(scroll);
        lv_obj_set_width(ctx->contentWidget_, LV_PCT(100));
        lv_label_set_long_mode(ctx->contentWidget_, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(ctx->contentWidget_, selectContentFont(false, false), 0);
        lv_label_set_text(ctx->contentWidget_, ctx->pageContent_.c_str());
    } else if (ctx->epub_) {
        // EPUB: transparent flex-column container; renderPage() fills it with
        // per-paragraph labels (one per paragraph, with individual alignment).
        ctx->contentWidget_ = lv_obj_create(scroll);
        lv_obj_set_width(ctx->contentWidget_, LV_PCT(100));
        lv_obj_set_height(ctx->contentWidget_, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(ctx->contentWidget_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(ctx->contentWidget_, 0, 0);
        // One line-height of gap between paragraphs keeps all paragraph tops on
        // exact lineH-multiple boundaries - required for snapStep to work cleanly.
        lv_obj_set_style_pad_row(ctx->contentWidget_,
            (lv_coord_t)lv_font_get_line_height(selectContentFont(false, false)), 0);
        lv_obj_set_style_border_width(ctx->contentWidget_, 0, 0);
        lv_obj_set_style_bg_opa(ctx->contentWidget_, LV_OPA_TRANSP, 0);
        lv_obj_set_scrollable(ctx->contentWidget_, false);
        lv_obj_set_clickable(ctx->contentWidget_, false);
        loadChapter(ctx, ctx->currentSpineIndex_, +1);
    }

    lv_obj_update_layout(scroll);
    lv_obj_scroll_to_y(scroll, (int32_t)std::min<size_t>(offset, LV_COORD_MAX), LV_ANIM_OFF);
    ctx->pageOffset_ = (size_t)std::max<int32_t>(lv_obj_get_scroll_y(scroll), 0);
    saveProgress(ctx);

    // Keys turn the pages
    lv_group_t* group = lv_group_get_default();
    if (group != nullptr && (device_has_active_by_type(&KEYBOARD_TYPE) || device_has_active_by_type(&TRACKBALL_TYPE))) {
        lv_group_add_obj(group, scroll);
        lv_group_focus_obj(scroll);
        lv_group_set_editing(group, true);
    }
}

static lv_obj_t* addToolbarIcon(Context* ctx, const char* symbol, lv_event_cb_t callback) {
    return lvgl_toolbar_add_text_button_action(ctx->toolbar_, symbol, callback, ctx);
}

void showView(Context* ctx) {
    if (!ctx->wrapperWidget_ || !ctx->toolbar_) return;

    lv_group_t* group = lv_group_get_default();
    if (group != nullptr) lv_group_set_editing(group, false);
    ctx->contentWidget_ = nullptr;
    ctx->statusLabel_ = nullptr;
    lv_obj_clean(ctx->wrapperWidget_);
    lvgl_toolbar_clear_actions(ctx->toolbar_);

    if (ctx->view == View::Library) {
        lvgl_toolbar_set_title(ctx->toolbar_, "ePub Reader");
        lvgl_toolbar_set_nav_action(ctx->toolbar_, LV_SYMBOL_CLOSE, onExitPressed, ctx);
        buildLibraryUI(ctx, ctx->wrapperWidget_);
        return;
    }

    lvgl_toolbar_set_nav_action(ctx->toolbar_, LV_SYMBOL_LEFT, onBackPressed, ctx);
    if (ctx->view == View::Loading) {
        lvgl_toolbar_set_title(ctx->toolbar_, "ePub Reader");
        lv_obj_t* label = lv_label_create(ctx->wrapperWidget_);
        lv_obj_set_style_pad_all(label, 8, 0);
        lv_label_set_text(label, "Opening...");
        return;
    }

    std::string title = ctx->currentFilePath_.substr(ctx->currentFilePath_.find_last_of('/') + 1);
    if (ctx->epub_) {
        const std::string metadata = ctx->epub_->getMetadata("title");
        if (!metadata.empty()) title = metadata;
    }
    lvgl_toolbar_set_title(ctx->toolbar_, title.c_str());
    if (hasContents(ctx)) addToolbarIcon(ctx, LV_SYMBOL_LIST, onContentsPressed);
    addToolbarIcon(ctx, LV_SYMBOL_BARS, onMenuPressed);
    buildReaderUI(ctx, ctx->wrapperWidget_);
}

void epubReaderCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    // Hard requirement: without PSRAM this app cannot parse EPUBs or hold fonts.
    // main() shows an alert and then closes the app.
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
        return;
    }

    loadFonts(ctx);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(parent, false);
    lv_obj_set_style_pad_row(parent, 0, 0);

    ctx->toolbar_ = lvgl_toolbar_create(parent, "ePub Reader");

    ctx->wrapperWidget_ = lv_obj_create(parent);
    lv_obj_set_width(ctx->wrapperWidget_, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->wrapperWidget_, 1);
    lv_obj_set_flex_flow(ctx->wrapperWidget_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctx->wrapperWidget_, 0, 0);
    lv_obj_set_style_border_width(ctx->wrapperWidget_, 0, 0);
    lv_obj_set_style_bg_opa(ctx->wrapperWidget_, LV_OPA_TRANSP, 0);
    lv_obj_set_scrollable(ctx->wrapperWidget_, false);

    showView(ctx);
}

void epubReaderDestroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    rememberPosition(ctx);
    ctx->contentWidget_ = nullptr;
    ctx->statusLabel_ = nullptr;
    ctx->wrapperWidget_ = nullptr;
    ctx->toolbar_ = nullptr;
}
