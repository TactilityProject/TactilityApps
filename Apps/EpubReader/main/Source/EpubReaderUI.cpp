#include "EpubReader.h"
#include <tt_lvgl_toolbar.h>
#include <tactility/log.h>
#include <esp_heap_caps.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

static const char* TAG = "EpubReader";

// Runtime-loaded Noto Serif fonts - 4 variants for the active display size tier.
// Loaded by EpubReader::loadFonts() on onShow, freed by unloadFonts() on onHide
// (skipped when onHide fires for a dialog roundtrip - tocDialogId_ will be non-zero).
// lv_binfont_create/destroy are exported by the firmware's lvgl-module symbols.
// NOTE: These are file-scope statics intentionally. Tactility runs one instance of
// each app at a time, so there is no multi-instance aliasing or use-after-free risk.
static lv_font_t* s_fontRegular    = nullptr;
static lv_font_t* s_fontItalic     = nullptr;
static lv_font_t* s_fontBold       = nullptr;
static lv_font_t* s_fontBoldItalic = nullptr;

#define LVGL_SYMBOL_BOOK "\xEF\x94\xBE"   // U+F53E (MaterialSymbols: book_2)
#define LVGL_SYMBOL_TEXT "\xEF\x87\x86"   // U+F1C6 (MaterialSymbols: text_snippet)

// lv_list_add_btn places the icon image at child 0 and the text label at child 1.
// Centralise that assumption here so there's one place to update if LVGL changes.
static void setListBtnLongMode(lv_obj_t* btn, lv_label_long_mode_t mode) {
    lv_obj_t* lbl = lv_obj_get_child(btn, 1);
    if (lbl) lv_label_set_long_mode(lbl, mode);
}

// ---------------------------------------------------------------------------
// Toolbar helper
// ---------------------------------------------------------------------------

void EpubReader::setReaderToolbarButtons() {
    tt_lvgl_toolbar_clear_actions(toolbar_);
    tt_lvgl_toolbar_add_text_button_action(toolbar_, LV_SYMBOL_PREV, onPrevPressed, this);
    if (!textMode_) {
        tt_lvgl_toolbar_add_text_button_action(toolbar_, LV_SYMBOL_LIST, onTocPressed, this);
    }
    tt_lvgl_toolbar_add_text_button_action(toolbar_, LV_SYMBOL_NEXT, onNextPressed, this);
    tt_lvgl_toolbar_add_text_button_action(toolbar_, LV_SYMBOL_DIRECTORY, onBrowsePressed, this);
}

void EpubReader::setBrowserToolbarButtons() {
    tt_lvgl_toolbar_clear_actions(toolbar_);
    // Show "Use Folder" button when the current browse path isn't already the saved books folder
    if (browsePath_ != booksPath_) {
        tt_lvgl_toolbar_add_text_button_action(toolbar_, LV_SYMBOL_DIRECTORY, onSetBooksFolder, this);
    }
}

// ---------------------------------------------------------------------------
// UI builders
// ---------------------------------------------------------------------------

static const lv_font_t* selectIconFont() {
    lv_coord_t w = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t h = lv_display_get_vertical_resolution(nullptr);
    bool isLarge = w >= 480 || h >= 320;
    return isLarge ? &material_symbols_shared_32 : &material_symbols_shared_20;
}

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

void EpubReader::loadFonts() {
    if (s_fontRegular) return;  // already loaded

    // Without PSRAM the heap is too constrained to hold even a single binary font
    // alongside the epub parser and LVGL allocations - skip loading entirely.
    // onShow() will have already shown an alert dialog for this case.
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) return;

    // Verify the assets directory exists before attempting individual file loads.
    // This produces one clear diagnostic instead of one error per font file.
    char assetsDir[256]; size_t dirSz = sizeof(assetsDir);
    tt_app_get_assets_path(appHandle_, assetsDir, &dirSz);
    if (dirSz == 0) {
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
        char assetPath[256]; size_t pathSz = sizeof(assetPath);
        tt_app_get_assets_child_path(appHandle_, filename, assetPath, &pathSz);
        if (pathSz == 0) {
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

void EpubReader::unloadFonts() {
    lv_font_t** ptrs[] = { &s_fontRegular, &s_fontItalic, &s_fontBold, &s_fontBoldItalic };
    for (auto* p : ptrs) {
        if (*p) { lv_binfont_destroy(*p); *p = nullptr; }
    }
}

void EpubReader::buildReaderUI(lv_obj_t* parent) {
    lv_obj_t* scroll = lv_obj_create(parent);
    lv_obj_set_width(scroll, LV_PCT(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_pad_all(scroll, 6, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);

    // Tap left half = prev, right half = next.
    // LV_EVENT_CLICKED only fires on press+release with minimal movement,
    // so it won't conflict with vertical scrolling in text mode.
    lv_obj_add_event_cb(scroll, onReaderTap, LV_EVENT_CLICKED, this);

    if (textMode_) {
        // Plain text file: single label, Prev/Next scroll by screen height.
        contentWidget_ = lv_label_create(scroll);
        lv_obj_set_width(contentWidget_, LV_PCT(100));
        lv_label_set_long_mode(contentWidget_, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_text_font(contentWidget_, selectContentFont(false, false), 0);
        lv_label_set_text(contentWidget_, pageContent_.c_str());
        lv_obj_scroll_to_y(scroll, (lv_coord_t)pageOffset_, LV_ANIM_OFF);
        saveProgress();
    } else if (epub_) {
        // EPUB: transparent flex-column container; renderPage() fills it with
        // per-paragraph labels (one per paragraph, with individual alignment).
        contentWidget_ = lv_obj_create(scroll);
        lv_obj_set_width(contentWidget_, LV_PCT(100));
        lv_obj_set_height(contentWidget_, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(contentWidget_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(contentWidget_, 0, 0);
        // One line-height of gap between paragraphs keeps all paragraph tops on
        // exact lineH-multiple boundaries - required for snapStep to work cleanly.
        lv_obj_set_style_pad_row(contentWidget_,
            (lv_coord_t)lv_font_get_line_height(selectContentFont(false, false)), 0);
        lv_obj_set_style_border_width(contentWidget_, 0, 0);
        lv_obj_set_style_bg_opa(contentWidget_, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(contentWidget_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(contentWidget_, LV_OBJ_FLAG_CLICKABLE);
        loadChapter(currentSpineIndex_, +1);
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
    lv_obj_remove_flag(sg, LV_OBJ_FLAG_CLICKABLE);

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
// renderSlice - parse ESC-encoded text into per-paragraph LVGL widgets
// ---------------------------------------------------------------------------
// ESC token protocol (see HtmlStrip.h):
//   Every paragraph starts with ESC + 'L'|'C'|'R' (alignment).
//   Paragraphs are separated by '\n\n'.
//   Single '\n' = line break within a paragraph.
//   Inline ESC+'B'/'b'/'I'/'i' tokens delimit bold/italic runs.
//
// Paragraphs without inline tokens → lv_label (lighter, fewer allocations).
// Paragraphs with inline tokens    → lv_spangroup (per-span font selection).
void EpubReader::renderSlice(const std::string& slice) {
    if (!contentWidget_) return;

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
                renderSpanParagraph(contentWidget_, seg, segLen, align);
            } else {
                // Plain paragraph: lv_label (simpler, lower memory overhead)
                lv_obj_t* lbl = lv_label_create(contentWidget_);
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

void EpubReader::buildBrowserUI(lv_obj_t* parent) {
    // When at the configured books folder, show the shelf instead of the file list
    if (!booksPath_.empty() && browsePath_ == booksPath_) {
        buildShelfUI(parent);
        return;
    }

    // Path bar
    lv_obj_t* pathBar = lv_obj_create(parent);
    lv_obj_set_size(pathBar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pathBar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pathBar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(pathBar, 4, 0);
    lv_obj_set_style_pad_gap(pathBar, 4, 0);
    lv_obj_set_style_border_width(pathBar, 0, 0);
    lv_obj_set_style_bg_opa(pathBar, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(pathBar, LV_OBJ_FLAG_SCROLLABLE);

    if (browsePath_ != dataRoot_) {
        lv_obj_t* backBtn = lv_button_create(pathBar);
        lv_obj_set_size(backBtn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(backBtn, 4, 0);
        lv_label_set_text(lv_label_create(backBtn), LV_SYMBOL_LEFT " Back");
        lv_obj_add_event_cb(backBtn, onBrowserBack, LV_EVENT_CLICKED, this);
    }

    lv_obj_t* pathLabel = lv_label_create(pathBar);
    lv_obj_set_flex_grow(pathLabel, 1);
    lv_label_set_text(pathLabel, browsePath_.c_str());
    lv_label_set_long_mode(pathLabel, LV_LABEL_LONG_MODE_DOTS);//LV_LABEL_LONG_MODE_SCROLL_CIRCULAR

    // File list
    lv_obj_t* list = lv_list_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_border_width(list, 0, 0);

    // Scan directory
    browserEntries_.clear();
    DIR* dir = opendir(browsePath_.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_name[0] == '.') continue;
            std::string name(entry->d_name);
            bool isDir = (entry->d_type == DT_DIR);
            if (entry->d_type == DT_UNKNOWN) {
                struct stat st;
                std::string fullPath = browsePath_ + "/" + name;
                if (stat(fullPath.c_str(), &st) == 0) {
                    isDir = S_ISDIR(st.st_mode);
                }
            }
            if (isDir || isSupportedFile(name)) {
                browserEntries_.push_back({name, isDir});
            }
        }
        closedir(dir);
    } else {
        LOG_W(TAG, "Cannot open dir: %s", browsePath_.c_str());
    }

    // Sort: directories first, then alphabetically within each group
    std::sort(browserEntries_.begin(), browserEntries_.end(),
        [](const auto& a, const auto& b) {
            if (a.second != b.second) return a.second > b.second;
            return a.first < b.first;
        });

    if (browserEntries_.empty()) {
        lv_list_add_text(list, "No supported files found.");
    } else {
        for (size_t i = 0; i < browserEntries_.size(); ++i) {
            const auto& [name, isDir] = browserEntries_[i];
            const char* icon = isDir ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE;
            lv_obj_t* btn = lv_list_add_button(list, icon, name.c_str());
            lv_obj_set_user_data(btn, (void*)(uintptr_t)i);
            lv_obj_add_event_cb(btn, onBrowserItem, LV_EVENT_CLICKED, this);
            setListBtnLongMode(btn, LV_LABEL_LONG_MODE_DOTS);//LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
        }
    }
}

// Recursively scans path for epub/txt files, appending entries to browserEntries_.
// Called with prefix="" for booksPath_; recurses one level into subdirectories.
void EpubReader::scanBooksDir(const std::string& path, const std::string& prefix) {
    DIR* d = opendir(path.c_str());
    if (!d) return;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string name(ent->d_name);
        bool isDir = (ent->d_type == DT_DIR);
        if (ent->d_type == DT_UNKNOWN) {
            struct stat st;
            if (stat((path + "/" + name).c_str(), &st) == 0)
                isDir = S_ISDIR(st.st_mode);
        }
        if (isDir && prefix.empty()) {
            scanBooksDir(path + "/" + name, name + "/");
        } else if (!isDir) {
            auto p = name.rfind('.');
            if (p == std::string::npos) continue;
            std::string e = name.substr(p);
            std::transform(e.begin(), e.end(), e.begin(), ::tolower);
            if (e == ".epub" || e == ".txt")
                browserEntries_.push_back({prefix + name, false});
        }
    }
    closedir(d);
}

// Books shelf view - shown when browsePath_ == booksPath_.
// Scans booksPath_ and one level of subdirectories into a single flat sorted list.
void EpubReader::buildShelfUI(lv_obj_t* parent) {
    lv_coord_t dispW = lv_display_get_horizontal_resolution(nullptr);
    lv_coord_t dispH = lv_display_get_vertical_resolution(nullptr);
    bool isLarge  = (dispW >= 480 || dispH >= 480);
    bool isXLarge = (dispW >= 600 || dispH >= 600);

    int pageSize   = isXLarge ? 8 : (isLarge ? 6 : 4);
    int padBar     = isLarge ? 4 : 2;
    int navPadHor  = isLarge ? 12 : 6;
    int navPadVer  = isLarge ? 6 : 3;

    // Optional path bar with Back so the user can navigate away from the shelf
    if (booksPath_ != dataRoot_) {
        lv_obj_t* pathBar = lv_obj_create(parent);
        lv_obj_set_size(pathBar, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(pathBar, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(pathBar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(pathBar, padBar, 0);
        lv_obj_set_style_pad_gap(pathBar, padBar, 0);
        lv_obj_set_style_border_width(pathBar, 0, 0);
        lv_obj_set_style_bg_opa(pathBar, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(pathBar, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* backBtn = lv_button_create(pathBar);
        lv_obj_set_size(backBtn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(backBtn, padBar, 0);
        lv_label_set_text(lv_label_create(backBtn), LV_SYMBOL_LEFT " Browse");
        lv_obj_add_event_cb(backBtn, onBrowserBack, LV_EVENT_CLICKED, this);

        lv_obj_t* pathLbl = lv_label_create(pathBar);
        lv_obj_set_flex_grow(pathLbl, 1);
        lv_label_set_long_mode(pathLbl, LV_LABEL_LONG_MODE_DOTS);//LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
        lv_label_set_text(pathLbl, booksPath_.c_str());
    }

    // Scan booksPath_ for books, and one level of subdirectories into a flat list.
    // Subfolder entries are stored as "subfolder/filename" so onBrowserItem builds
    // the correct full path via browsePath_ + "/" + name.
    browserEntries_.clear();
    scanBooksDir(booksPath_, "");

    // Sort alphabetically by filename (ignoring subfolder prefix)
    std::sort(browserEntries_.begin(), browserEntries_.end(),
        [](const auto& a, const auto& b) {
            // rfind returns npos if no '/' found; npos+1 wraps to 0, yielding the full string
            auto nameA = a.first.substr(a.first.rfind('/') + 1);
            auto nameB = b.first.substr(b.first.rfind('/') + 1);
            return std::lexicographical_compare(
                nameA.begin(), nameA.end(),
                nameB.begin(), nameB.end(),
                [](char ca, char cb) { return std::tolower((unsigned char)ca) < std::tolower((unsigned char)cb); });
        });

    // Clamp shelfPage_ to valid range
    int totalItems = (int)browserEntries_.size();
    int totalPages = std::max(1, (totalItems + pageSize - 1) / pageSize);
    shelfPage_ = std::max(0, std::min(shelfPage_, totalPages - 1));
    int startIdx = shelfPage_ * pageSize;
    int endIdx   = std::min(startIdx + pageSize, totalItems);

    // Non-scrollable list; items share height equally via flex_grow=1
    lv_obj_t* list = lv_list_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLLABLE);

    if (browserEntries_.empty()) {
        lv_list_add_text(list, "No books found in books folder.");
    } else {
        for (int i = startIdx; i < endIdx; ++i) {
            const std::string& entry = browserEntries_[(size_t)i].first;
            // Display name: strip subfolder prefix and extension
            std::string displayName = entry.substr(entry.rfind('/') + 1);
            auto dot = displayName.rfind('.');
            if (dot != std::string::npos) displayName = displayName.substr(0, dot);

            auto dotPos = entry.rfind('.');
            std::string ext = (dotPos != std::string::npos) ? entry.substr(dotPos) : "";
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            const char* icon = (ext == ".txt") ? LVGL_SYMBOL_TEXT : LVGL_SYMBOL_BOOK;

            lv_obj_t* btn = lv_list_add_button(list, icon, displayName.c_str());
            lv_obj_set_flex_grow(btn, 1);  // equal height share across page items
            lv_obj_set_user_data(btn, (void*)(uintptr_t)i);
            lv_obj_add_event_cb(btn, onBrowserItem, LV_EVENT_CLICKED, this);
            setListBtnLongMode(btn, LV_LABEL_LONG_MODE_DOTS);//LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
            lv_obj_t* iconLbl = lv_obj_get_child(btn, 0);
            if (iconLbl) lv_obj_set_style_text_font(iconLbl, selectIconFont(), 0);
        }
    }

    // ── Bottom navigation bar ─────────────────────────────────────────────────
    lv_obj_t* navBar = lv_obj_create(parent);
    lv_obj_set_size(navBar, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(navBar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(navBar, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(navBar, padBar, 0);
    lv_obj_set_style_pad_gap(navBar, padBar, 0);
    lv_obj_set_style_border_width(navBar, 0, 0);
    lv_obj_set_style_bg_opa(navBar, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(navBar, LV_OBJ_FLAG_SCROLLABLE);

    bool canPrev = (shelfPage_ > 0);
    bool canNext = (shelfPage_ < totalPages - 1);

    auto makeNavBtn = [&](const char* label, lv_event_cb_t cb, bool enabled) {
        lv_obj_t* btn = lv_button_create(navBar);
        lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(btn, navPadHor, 0);
        lv_obj_set_style_pad_ver(btn, navPadVer, 0);
        lv_label_set_text(lv_label_create(btn), label);
        if (enabled) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, this);
        else         lv_obj_add_state(btn, LV_STATE_DISABLED);
    };

    makeNavBtn(LV_SYMBOL_PREV, onShelfFirst, canPrev);
    makeNavBtn(LV_SYMBOL_LEFT, onShelfPrev,  canPrev);

    char pageBuf[24];
    snprintf(pageBuf, sizeof(pageBuf), "%d / %d", shelfPage_ + 1, totalPages);
    lv_obj_t* pageLbl = lv_label_create(navBar);
    lv_obj_set_style_pad_hor(pageLbl, 10, 0);
    lv_label_set_text(pageLbl, pageBuf);

    makeNavBtn(LV_SYMBOL_RIGHT, onShelfNext, canNext);
    makeNavBtn(LV_SYMBOL_NEXT,  onShelfLast, canNext);
}
