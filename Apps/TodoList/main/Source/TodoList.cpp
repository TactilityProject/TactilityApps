#include "TodoList.h"
#include <app/paths.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/filesystem/file_mutex.h>
#include <Tactility/kernel/Kernel.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl/lvgl.h>
#include <lvgl/fonts.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

constexpr const char* SAVE_FILENAME = "todos.txt";
/** Must match manifest.properties' app.id */
constexpr const char* APP_ID = "one.tactility.todolist";

// Attached to each list row / delete button so the click handlers know both which Context and
// which item index they belong to (mirrors AlertDialog/SelectionDialog's ButtonContext/
// ItemContext idiom). Freed via LV_EVENT_DELETE when rebuildList() cleans the list.
struct ItemContext {
    Context* ctx;
    int index;
};

/* ── Helpers ──────────────────────────────────────────────────────── */

bool getSaveFilePath(char* buf, size_t bufSize) {
    return app_paths_get_user_data_path(APP_ID, SAVE_FILENAME, buf, bufSize) == ERROR_NONE;
}

bool ensureDir() {
    char dir[256];
    if (app_paths_get_user_data_directory(APP_ID, dir, sizeof(dir)) != ERROR_NONE) return false;
    for (char* p = dir + 1; *p; ++p) {
        if (*p == '/') { *p = '\0'; mkdir(dir, 0755); *p = '/'; }
    }
    mkdir(dir, 0755);
    return true;
}

uint32_t getToolbarHeight(UiDensity uiDensity) {
    if (uiDensity == LVGL_UI_DENSITY_COMPACT) {
        return lvgl_get_text_font_height(FONT_SIZE_DEFAULT) * 1.4f;
    } else {
        return lvgl_get_text_font_height(FONT_SIZE_LARGE) * 2.2f;
    }
}

uint32_t getActionIconPadding(UiDensity uiDensity) {
    auto toolbar_height = getToolbarHeight(uiDensity);
    return (uiDensity != LVGL_UI_DENSITY_COMPACT) ? (uint32_t)(toolbar_height * 0.2f) : 8;
}

/* ── Persistence ──────────────────────────────────────────────────── */

void saveTodos(Context* ctx) {
    if (!ensureDir()) return;
    char savePath[256];
    if (!getSaveFilePath(savePath, sizeof(savePath))) return;

    struct FileMutex mutex;
    file_mutex_get(&mutex, savePath);
    file_mutex_lock(&mutex);
    FILE* f = fopen(savePath, "w");
    if (f) {
        for (int i = 0; i < ctx->count; i++) {
            fprintf(f, "%c %s\n", ctx->items[i].done ? '+' : '-', ctx->items[i].text);
        }
        fclose(f);
    }
    file_mutex_unlock(&mutex);
}

void loadTodos(Context* ctx) {
    char savePath[256];
    if (!getSaveFilePath(savePath, sizeof(savePath))) return;

    struct FileMutex mutex;
    file_mutex_get(&mutex, savePath);

    file_mutex_lock(&mutex);
    ctx->count = 0;
    FILE* f = fopen(savePath, "r");
    if (f) {
        char line[Context::MAX_TEXT_LEN + 4];
        while (ctx->count < Context::MAX_TODOS && fgets(line, sizeof(line), f)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[--len] = '\0';
            }

            if (len < 3 || line[1] != ' ') continue;

            Context::TodoItem* item = &ctx->items[ctx->count];
            item->done = (line[0] == '+');
            strncpy(item->text, &line[2], Context::MAX_TEXT_LEN - 1);
            item->text[Context::MAX_TEXT_LEN - 1] = '\0';
            ctx->count++;
        }
        fclose(f);
    }
    file_mutex_unlock(&mutex);
}

/* ── UI Helpers ───────────────────────────────────────────────────── */

void updateCountLabel(Context* ctx) {
    if (!ctx->countLabel) return;
    int pending = 0;
    for (int i = 0; i < ctx->count; i++) {
        if (!ctx->items[i].done) pending++;
    }
    if (pending > 0) {
        lv_label_set_text_fmt(ctx->countLabel, "%d left", pending);
    } else if (ctx->count > 0) {
        lv_label_set_text(ctx->countLabel, "All done!");
    } else {
        lv_label_set_text(ctx->countLabel, "No tasks");
    }
}

void onItemClicked(lv_event_t* e);
void onDeleteClicked(lv_event_t* e);

void onItemContextDeleted(lv_event_t* e) {
    delete static_cast<ItemContext*>(lv_event_get_user_data(e));
}

void rebuildList(Context* ctx) {
    if (!ctx->list) return;

    lv_obj_clean(ctx->list);

    auto ui_density = lvgl_get_ui_density();
    auto toolbar_height = getToolbarHeight(ui_density);
    auto icon_padding = getActionIconPadding(ui_density);

    if (ctx->count == 0) {
        lv_list_add_text(ctx->list, "No tasks yet. Add one below!");
    }

    for (int i = 0; i < ctx->count; i++) {
        Context::TodoItem* item = &ctx->items[i];

        char display[Context::MAX_TEXT_LEN + 8];
        snprintf(display, sizeof(display), "%s %s", item->done ? LV_SYMBOL_OK : LV_SYMBOL_DUMMY, item->text);

        lv_obj_t* btn = lv_list_add_button(ctx->list, NULL, display);

        if (item->done) {
            lv_obj_set_style_text_opa(btn, LV_OPA_50, LV_PART_MAIN);
        }

        auto* itemCtx = new ItemContext { ctx, i };
        lv_obj_add_event_cb(btn, onItemClicked, LV_EVENT_SHORT_CLICKED, itemCtx);
        lv_obj_add_event_cb(btn, onItemContextDeleted, LV_EVENT_DELETE, itemCtx);

        lv_obj_t* delBtn = lv_button_create(btn);
        lv_obj_set_size(delBtn, toolbar_height - icon_padding, toolbar_height - icon_padding);
        lv_obj_align(delBtn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(delBtn, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
        lv_obj_set_style_pad_all(delBtn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(delBtn, 0, LV_PART_MAIN);

        lv_obj_t* delIcon = lv_label_create(delBtn);
        lv_label_set_text(delIcon, LV_SYMBOL_CLOSE);
        lv_obj_center(delIcon);

        auto* delItemCtx = new ItemContext { ctx, i };
        lv_obj_add_event_cb(delBtn, onDeleteClicked, LV_EVENT_CLICKED, delItemCtx);
        lv_obj_add_event_cb(delBtn, onItemContextDeleted, LV_EVENT_DELETE, delItemCtx);
    }

    updateCountLabel(ctx);
}

void onDeferredRebuild(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    ctx->rebuildTimer = nullptr;
    ctx->rebuildPending = false;

    // list only exists while this window is topmost - skip otherwise (window_manager deletes a
    // buried window's widgets; same reasoning as GPIO.cpp's periodic status timer).
    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) return;

    rebuildList(ctx);
}

void scheduleRebuild(Context* ctx) {
    if (ctx->rebuildPending) return;
    ctx->rebuildPending = true;
    ctx->rebuildTimer = lv_timer_create(onDeferredRebuild, 0, ctx);
    if (!ctx->rebuildTimer) {
        ctx->rebuildPending = false;
        return;
    }
    lv_timer_set_repeat_count(ctx->rebuildTimer, 1);
}

void addItem(Context* ctx, const char* text) {
    if (!text || !text[0]) return;
    if (ctx->count >= Context::MAX_TODOS) return;

    while (*text == ' ') text++;
    if (!*text) return;

    Context::TodoItem* item = &ctx->items[ctx->count];
    item->done = false;
    strncpy(item->text, text, Context::MAX_TEXT_LEN - 1);
    item->text[Context::MAX_TEXT_LEN - 1] = '\0';

    size_t len = strlen(item->text);
    while (len > 0 && item->text[len - 1] == ' ') {
        item->text[--len] = '\0';
    }

    ctx->count++;
    saveTodos(ctx);
    rebuildList(ctx);
}

/* ── Callbacks ────────────────────────────────────────────────────── */

void onItemClicked(lv_event_t* e) {
    auto* itemCtx = static_cast<ItemContext*>(lv_event_get_user_data(e));
    Context* ctx = itemCtx->ctx;
    int idx = itemCtx->index;
    if (idx < 0 || idx >= ctx->count) return;

    ctx->items[idx].done = !ctx->items[idx].done;
    saveTodos(ctx);
    scheduleRebuild(ctx);
}

void onDeleteClicked(lv_event_t* e) {
    auto* itemCtx = static_cast<ItemContext*>(lv_event_get_user_data(e));
    Context* ctx = itemCtx->ctx;
    int idx = itemCtx->index;
    if (idx < 0 || idx >= ctx->count) return;

    for (int i = idx; i < ctx->count - 1; i++) {
        ctx->items[i] = ctx->items[i + 1];
    }
    ctx->count--;

    saveTodos(ctx);
    scheduleRebuild(ctx);
}

void onAddClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (!ctx->inputTa) return;
    const char* text = lv_textarea_get_text(ctx->inputTa);
    addItem(ctx, text);
    lv_textarea_set_text(ctx->inputTa, "");
}

void onInputReady(lv_event_t* e) {
    onAddClicked(e);
}

void onClearDoneClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    int write = 0;
    for (int read = 0; read < ctx->count; read++) {
        if (!ctx->items[read].done) {
            if (write != read) {
                ctx->items[write] = ctx->items[read];
            }
            write++;
        }
    }
    ctx->count = write;
    saveTodos(ctx);
    scheduleRebuild(ctx);
}

} // namespace

/* ── Lifecycle ────────────────────────────────────────────────────── */

void todoListCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    loadTodos(ctx);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    /* Toolbar */
    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Todo List");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* countWrapper = lv_obj_create(toolbar);
    lv_obj_set_size(countWrapper, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_pad_top(countWrapper, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_bottom(countWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_left(countWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_right(countWrapper, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_row(countWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(countWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(countWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(countWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_remove_flag(countWrapper, LV_OBJ_FLAG_SCROLLABLE);

    ctx->countLabel = lv_label_create(countWrapper);
    lv_obj_set_size(ctx->countLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(ctx->countLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(ctx->countLabel, LV_TEXT_ALIGN_LEFT, LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(ctx->countLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(ctx->countLabel, lv_palette_main(LV_PALETTE_CYAN), LV_PART_MAIN);

    auto ui_density = lvgl_get_ui_density();
    auto toolbar_height = getToolbarHeight(ui_density);
    auto icon_padding = getActionIconPadding(ui_density);

    lv_obj_t* btnWrapper = lv_obj_create(toolbar);
    lv_obj_set_width(btnWrapper, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnWrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btnWrapper, icon_padding / 2, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(btnWrapper, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btnWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btnWrapper, 0, LV_STATE_DEFAULT);

    lv_obj_t* clearBtn = lv_button_create(btnWrapper);
    lv_obj_set_size(clearBtn, toolbar_height - icon_padding, toolbar_height - icon_padding);
    lv_obj_set_style_pad_all(clearBtn, 0, LV_STATE_DEFAULT);
    lv_obj_align(clearBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(clearBtn, onClearDoneClicked, LV_EVENT_CLICKED, ctx);

    lv_obj_t* clearIcon = lv_label_create(clearBtn);
    lv_label_set_text(clearIcon, LV_SYMBOL_TRASH);
    lv_obj_align(clearIcon, LV_ALIGN_CENTER, 0, 0);

    /* Container */
    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_set_style_pad_gap(cont, 4, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(cont, 0, 0);

    /* Scrollable list */
    ctx->list = lv_list_create(cont);
    lv_obj_set_width(ctx->list, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->list, 1);

    /* Input row */
    ctx->inputRow = lv_obj_create(cont);
    lv_obj_set_size(ctx->inputRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctx->inputRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctx->inputRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(ctx->inputRow, 0, 0);
    lv_obj_set_style_pad_gap(ctx->inputRow, 4, 0);
    lv_obj_remove_flag(ctx->inputRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(ctx->inputRow, 0, 0);

    ctx->inputTa = lv_textarea_create(ctx->inputRow);
    lv_textarea_set_placeholder_text(ctx->inputTa, "New task...");
    lv_textarea_set_one_line(ctx->inputTa, true);
    lv_obj_set_flex_grow(ctx->inputTa, 1);
    lv_obj_set_style_text_font(ctx->inputTa, lv_font_get_default(), 0);
    lv_obj_add_event_cb(ctx->inputTa, onInputReady, LV_EVENT_READY, ctx);

    lv_obj_t* addBtn = lv_button_create(ctx->inputRow);
    lv_obj_t* addLbl = lv_label_create(addBtn);
    lv_label_set_text(addLbl, LV_SYMBOL_PLUS);
    lv_obj_add_event_cb(addBtn, onAddClicked, LV_EVENT_CLICKED, ctx);

    rebuildList(ctx);
}

void todoListTeardown(Context* ctx) {
    if (ctx->rebuildTimer) {
        lv_timer_delete(ctx->rebuildTimer);
        ctx->rebuildTimer = nullptr;
    }
    ctx->rebuildPending = false;
    ctx->list = nullptr;
    ctx->inputRow = nullptr;
    ctx->inputTa = nullptr;
    ctx->countLabel = nullptr;
}
