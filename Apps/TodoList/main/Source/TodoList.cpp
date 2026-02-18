#include "TodoList.h"
#include <tt_lock.h>
#include <Tactility/kernel/Kernel.h>
#include <tt_lvgl_toolbar.h>
#include <tt_lvgl_keyboard.h>
#include <tactility/lvgl_module.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr const char* SAVE_DIR = "/sdcard/tactility/todolist";
static constexpr const char* SAVE_FILE = "/sdcard/tactility/todolist/todos.txt";

/* File-scope instance pointer for index-based callbacks */
static TodoList* g_instance = nullptr;

/* ── Helpers ──────────────────────────────────────────────────────── */

static void ensureDir() {
    mkdir("/sdcard/tactility", 0755);
    mkdir(SAVE_DIR, 0755);
}

static int getToolbarHeight(UiDensity density) {
    return (density == LVGL_UI_DENSITY_COMPACT) ? 22 : 40;
}

/* ── Persistence ──────────────────────────────────────────────────── */

void TodoList::saveTodos() {
    ensureDir();
    auto lock = tt_lock_alloc_for_path(SAVE_FILE);
    if (!lock) return;
    if (tt_lock_acquire(lock, tt::kernel::MAX_TICKS)) {
        FILE* f = fopen(SAVE_FILE, "w");
        if (f) {
            for (int i = 0; i < count; i++) {
                fprintf(f, "%c %s\n", items[i].done ? '+' : '-', items[i].text);
            }
            fflush(f);
            fsync(fileno(f));
            fclose(f);
        }
        tt_lock_release(lock);
    }
    tt_lock_free(lock);
}

void TodoList::loadTodos() {
    auto lock = tt_lock_alloc_for_path(SAVE_FILE);
    if (!lock) return;

    if (tt_lock_acquire(lock, tt::kernel::MAX_TICKS)) {
        count = 0;
        FILE* f = fopen(SAVE_FILE, "r");
        if (f) {
            char line[MAX_TEXT_LEN + 4];
            while (count < MAX_TODOS && fgets(line, sizeof(line), f)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                    line[--len] = '\0';
                }

                if (len < 3 || line[1] != ' ') continue;

                TodoItem* item = &items[count];
                item->done = (line[0] == '+');
                strncpy(item->text, &line[2], MAX_TEXT_LEN - 1);
                item->text[MAX_TEXT_LEN - 1] = '\0';
                count++;
            }
            fclose(f);
        }
        tt_lock_release(lock);
    }
    tt_lock_free(lock);
}

/* ── UI Helpers ───────────────────────────────────────────────────── */

void TodoList::updateCountLabel() {
    if (!countLabel) return;
    int pending = 0;
    for (int i = 0; i < count; i++) {
        if (!items[i].done) pending++;
    }
    if (pending > 0) {
        lv_label_set_text_fmt(countLabel, "%d left", pending);
    } else if (count > 0) {
        lv_label_set_text(countLabel, "All done!");
    } else {
        lv_label_set_text(countLabel, "No tasks");
    }
}

void TodoList::addItem(const char* text) {
    if (!text || !text[0]) return;
    if (count >= MAX_TODOS) return;

    while (*text == ' ') text++;
    if (!*text) return;

    TodoItem* item = &items[count];
    item->done = false;
    strncpy(item->text, text, MAX_TEXT_LEN - 1);
    item->text[MAX_TEXT_LEN - 1] = '\0';

    size_t len = strlen(item->text);
    while (len > 0 && item->text[len - 1] == ' ') {
        item->text[--len] = '\0';
    }

    count++;
    saveTodos();
    rebuildList();
}

void TodoList::scheduleRebuild() {
    if (rebuildPending) return;
    rebuildPending = true;
    rebuildTimer = lv_timer_create(onDeferredRebuild, 0, this);
    lv_timer_set_repeat_count(rebuildTimer, 1);
}

void TodoList::onDeferredRebuild(lv_timer_t* timer) {
    TodoList* self = static_cast<TodoList*>(lv_timer_get_user_data(timer));
    if (self) {
        self->rebuildTimer = nullptr;
        self->rebuildPending = false;
        self->rebuildList();
    }
}

void TodoList::rebuildList() {
    if (!list) return;

    lv_obj_clean(list);

    UiDensity uiDensity = lvgl_get_ui_density();
    int toolbarHeight = getToolbarHeight(uiDensity);
    int btnSize = (uiDensity == LVGL_UI_DENSITY_COMPACT) ? toolbarHeight - 8 : toolbarHeight - 6;

    if (count == 0) {
        lv_list_add_text(list, "No tasks yet. Add one below!");
    }

    for (int i = 0; i < count; i++) {
        TodoItem* item = &items[i];

        char display[MAX_TEXT_LEN + 8];
        snprintf(display, sizeof(display), "%s %s", item->done ? LV_SYMBOL_OK : LV_SYMBOL_DUMMY, item->text);

        lv_obj_t* btn = lv_list_add_button(list, NULL, display);

        if (item->done) {
            lv_obj_set_style_text_opa(btn, LV_OPA_50, LV_PART_MAIN);
        }

        lv_obj_add_event_cb(btn, onItemClicked, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* delBtn = lv_button_create(btn);
        lv_obj_set_size(delBtn, btnSize, btnSize);
        lv_obj_align(delBtn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_bg_color(delBtn, lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
        lv_obj_set_style_pad_all(delBtn, 0, LV_PART_MAIN);
        lv_obj_set_style_shadow_width(delBtn, 0, LV_PART_MAIN);

        lv_obj_t* delIcon = lv_label_create(delBtn);
        lv_label_set_text(delIcon, LV_SYMBOL_CLOSE);
        lv_obj_center(delIcon);

        lv_obj_add_event_cb(delBtn, onDeleteClicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    updateCountLabel();
}

/* ── Callbacks ────────────────────────────────────────────────────── */

void TodoList::onItemClicked(lv_event_t* e) {
    if (!g_instance) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_instance->count) return;

    g_instance->items[idx].done = !g_instance->items[idx].done;
    g_instance->saveTodos();
    g_instance->scheduleRebuild();
}

void TodoList::onDeleteClicked(lv_event_t* e) {
    if (!g_instance) return;

    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_instance->count) return;

    for (int i = idx; i < g_instance->count - 1; i++) {
        g_instance->items[i] = g_instance->items[i + 1];
    }
    g_instance->count--;

    g_instance->saveTodos();
    g_instance->scheduleRebuild();
}

void TodoList::onAddClicked(lv_event_t* e) {
    if (!g_instance || !g_instance->inputTa) return;
    const char* text = lv_textarea_get_text(g_instance->inputTa);
    g_instance->addItem(text);
    lv_textarea_set_text(g_instance->inputTa, "");
}

void TodoList::onInputReady(lv_event_t* e) {
    onAddClicked(e);
}

void TodoList::onClearDoneClicked(lv_event_t* e) {
    if (!g_instance) return;
    int write = 0;
    for (int read = 0; read < g_instance->count; read++) {
        if (!g_instance->items[read].done) {
            if (write != read) {
                g_instance->items[write] = g_instance->items[read];
            }
            write++;
        }
    }
    g_instance->count = write;
    g_instance->saveTodos();
    g_instance->scheduleRebuild();
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

void TodoList::onShow(AppHandle app, lv_obj_t* parent) {
    g_instance = this;

    loadTodos();

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    /* Toolbar */
    lv_obj_t* toolbar = tt_lvgl_toolbar_create_for_app(parent, app);
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

    countLabel = lv_label_create(countWrapper);
    lv_obj_set_size(countLabel, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(countLabel, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_align(countLabel, LV_TEXT_ALIGN_LEFT, LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(countLabel, lv_font_get_default(), 0);
    lv_obj_set_style_text_color(countLabel, lv_palette_main(LV_PALETTE_CYAN), LV_PART_MAIN);

    UiDensity uiDensity = lvgl_get_ui_density();
    int toolbarHeight = getToolbarHeight(uiDensity);

    lv_obj_t* btnWrapper = lv_obj_create(toolbar);
    lv_obj_set_width(btnWrapper, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btnWrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(btnWrapper, 2, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_column(btnWrapper, 4, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btnWrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(btnWrapper, 0, LV_STATE_DEFAULT);

    int btnSize = (uiDensity == LVGL_UI_DENSITY_COMPACT)
        ? toolbarHeight - 8
        : toolbarHeight - 6;

    lv_obj_t* clearBtn = lv_button_create(btnWrapper);
    lv_obj_set_size(clearBtn, btnSize, btnSize);
    lv_obj_set_style_pad_all(clearBtn, 0, LV_STATE_DEFAULT);
    lv_obj_align(clearBtn, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_event_cb(clearBtn, onClearDoneClicked, LV_EVENT_CLICKED, nullptr);

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
    list = lv_list_create(cont);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);

    /* Input row */
    inputRow = lv_obj_create(cont);
    lv_obj_set_size(inputRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(inputRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(inputRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(inputRow, 0, 0);
    lv_obj_set_style_pad_gap(inputRow, 4, 0);
    lv_obj_remove_flag(inputRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(inputRow, 0, 0);

    inputTa = lv_textarea_create(inputRow);
    lv_textarea_set_placeholder_text(inputTa, "New task...");
    lv_textarea_set_one_line(inputTa, true);
    lv_obj_set_flex_grow(inputTa, 1);
    lv_obj_set_style_text_font(inputTa, lv_font_get_default(), 0);
    lv_obj_add_event_cb(inputTa, onInputReady, LV_EVENT_READY, nullptr);

    lv_obj_t* addBtn = lv_button_create(inputRow);
    lv_obj_t* addLbl = lv_label_create(addBtn);
    lv_label_set_text(addLbl, LV_SYMBOL_PLUS);
    lv_obj_add_event_cb(addBtn, onAddClicked, LV_EVENT_CLICKED, nullptr);

    rebuildList();
}

void TodoList::onHide(AppHandle app) {
    if (rebuildTimer) {
        lv_timer_delete(rebuildTimer);
        rebuildTimer = nullptr;
    }
    list = nullptr;
    inputRow = nullptr;
    inputTa = nullptr;
    countLabel = nullptr;
    g_instance = nullptr;
}
