#include "TodoList.h"

#include <app/event.h>
#include <lvgl/fonts.h>
#include <lvgl/grid_navigation.h>
#include <lvgl/icons/shared.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/list.h>
#include <lvgl/widgets/tabs.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/dialog.h>
#include <tactility/preferences.h>

#include <ctime>

namespace {

constexpr size_t MAX_TITLE_LENGTH = 128;
constexpr size_t MAX_DETAILS_LENGTH = 4096;

/* ── Actions ──────────────────────────────────────────────────────── */

/** Requires the LVGL lock */
void queue(Context* ctx, ActionType type, uint32_t value = 0, const std::string& text = "") {
    ctx->actions.push_back({ type, value, text });
    task_event_group_signal(ctx->eventGroup, ctx->actionBit);
}

/* ── State ────────────────────────────────────────────────────────── */

const std::string& currentGroup(const Context* ctx) {
    return ctx->groups[ctx->group];
}

Task* findTask(Context* ctx, uint32_t id) {
    for (auto& task : ctx->tasks) {
        if (task.id == id) return &task;
    }
    return nullptr;
}

void saveSettings(Context* ctx) {
    Preferences* preferences = preferences_open(ctx->store.getSettingsPath().c_str());
    if (preferences == nullptr) return;
    preferences_put_string(preferences, "group", currentGroup(ctx).c_str());
    preferences_put_bool(preferences, "hide_done", ctx->hideDone);
    preferences_close(preferences);
}

/** Reads the groups and the tasks of the one to select */
void reload(Context* ctx, std::string group) {
    ctx->groups = ctx->store.loadGroups();
    if (ctx->groups.empty()) {
        // The storage doesn't work
        ctx->storageFailed = true;
        ctx->groups.emplace_back(DEFAULT_GROUP);
    }
    ctx->group = 0;
    for (size_t i = 0; i < ctx->groups.size(); i++) {
        if (ctx->groups[i] == group) ctx->group = i;
    }
    ctx->tasks = ctx->store.loadTasks(currentGroup(ctx));
}

void saveTask(Context* ctx, Task& task) {
    if (!ctx->store.saveTask(currentGroup(ctx), task)) ctx->storageFailed = true;
}

/** Saves the detail page, when it shows something else than the task */
void saveOpenTask(Context* ctx) {
    Task* task = findTask(ctx, ctx->openTask);
    if (task == nullptr || ctx->titleInput == nullptr) return;
    std::string title = lv_textarea_get_text(ctx->titleInput);
    const std::string details = lv_textarea_get_text(ctx->detailsInput);
    // A task can't lose its title
    if (title.empty()) title = task->title;
    if (title == task->title && details == task->details) return;
    task->title = title;
    task->details = details;
    saveTask(ctx, *task);
}

/* ── Dialogs ──────────────────────────────────────────────────────── */

void onDialogResult(int32_t button, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    lvgl_lock();
    queue(ctx, ActionType::DialogResult, static_cast<uint32_t>(button));
    lvgl_unlock();
}

void onDialogText(bool confirmed, const char* text, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    lvgl_lock();
    // Cancelling is a button result, so it can't be confused with an empty name
    if (confirmed) queue(ctx, ActionType::DialogText, 0, text);
    else queue(ctx, ActionType::DialogResult, static_cast<uint32_t>(WINDOW_DIALOG_DISMISSED));
    lvgl_unlock();
}

void showSelection(Context* ctx, Dialog dialog, const char* title, const std::vector<const char*>& items) {
    ctx->dialog = dialog;
    if (window_manager_selection_dialog(ctx->appInstanceId, title, items.data(), static_cast<uint32_t>(items.size()), onDialogResult, ctx) != ERROR_NONE) {
        ctx->dialog = Dialog::None;
    }
}

void showConfirm(Context* ctx, Dialog dialog, const char* title, const std::string& message, const char* action) {
    const char* buttons[] = { action, "Cancel" };
    ctx->dialog = dialog;
    if (window_manager_alert_dialog(ctx->appInstanceId, title, message.c_str(), buttons, 2, onDialogResult, ctx) != ERROR_NONE) {
        ctx->dialog = Dialog::None;
    }
}

void showNameDialog(Context* ctx, Dialog dialog, const char* title, const char* name) {
    ctx->dialog = dialog;
    if (window_manager_input_dialog(ctx->appInstanceId, title, "Name of the group", name, "OK", "Cancel", onDialogText, ctx) != ERROR_NONE) {
        ctx->dialog = Dialog::None;
    }
}

void showDeleteGroupConfirm(Context* ctx) {
    showConfirm(ctx, Dialog::DeleteGroup, "Delete group",
        "Delete \"" + currentGroup(ctx) + "\" and its " + std::to_string(ctx->tasks.size()) + " task(s)?", "Delete");
}

/* ── Widget events ────────────────────────────────────────────────── */

uint32_t idOf(lv_obj_t* obj) {
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(obj)));
}

Context* contextOf(lv_event_t* e) {
    return static_cast<Context*>(lv_event_get_user_data(e));
}

void onExit(lv_event_t* e) {
    app_event_emit_close(contextOf(e)->appInstanceId);
}

void onBack(lv_event_t* e) {
    queue(contextOf(e), ActionType::CloseTask);
}

void onOverflow(lv_event_t* e) {
    queue(contextOf(e), ActionType::ShowOverflow);
}

void onDoneButton(lv_event_t* e) {
    auto* ctx = contextOf(e);
    queue(ctx, ActionType::ToggleTask, ctx->openTask);
}

void onTabChanged(lv_event_t* e) {
    auto* ctx = contextOf(e);
    queue(ctx, ActionType::SelectTab, lvgl_tabs_get_selected(ctx->tabs));
}

void onTabLongPressed(lv_event_t* e) {
    // The release would otherwise also click the tab
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    queue(contextOf(e), ActionType::ShowGroupMenu, idOf(lv_event_get_current_target_obj(e)));
}

void onTaskPressed(lv_event_t* e) {
    queue(contextOf(e), ActionType::OpenTask, idOf(lv_event_get_current_target_obj(e)));
}

void onTaskLongPressed(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (indev != nullptr) lv_indev_wait_release(indev);
    queue(contextOf(e), ActionType::ShowTaskMenu, idOf(lv_event_get_current_target_obj(e)));
}

void onTaskFocused(lv_event_t* e) {
    contextOf(e)->focusedTask = idOf(lv_event_get_current_target_obj(e));
}

void onCheckPressed(lv_event_t* e) {
    queue(contextOf(e), ActionType::ToggleTask, idOf(lv_event_get_current_target_obj(e)));
}

/** Space finishes the focused task, Delete removes it. Enter opens it, as it presses the row. */
void onListKey(lv_event_t* e) {
    auto* ctx = contextOf(e);
    const uint32_t key = lv_event_get_key(e);
    if (ctx->focusedTask == 0) return;
    if (key == ' ') {
        queue(ctx, ActionType::ToggleTask, ctx->focusedTask);
    } else if (key == LV_KEY_DEL || key == LV_KEY_BACKSPACE) {
        queue(ctx, ActionType::ConfirmDeleteTask, ctx->focusedTask);
    }
}

void onAdd(lv_event_t* e) {
    auto* ctx = contextOf(e);
    const std::string text = lv_textarea_get_text(ctx->input);
    if (text.find_first_not_of(' ') == std::string::npos) return;
    lv_textarea_set_text(ctx->input, "");
    queue(ctx, ActionType::AddTask, 0, text);
}

/* ── Views ────────────────────────────────────────────────────────── */

lv_obj_t* createPlain(lv_obj_t* parent, lv_flex_flow_t flow) {
    auto* obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_size(obj, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(obj, flow);
    return obj;
}

lv_obj_t* createLabel(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    return label;
}

void createRow(Context* ctx, const Task& task) {
    auto* row = lvgl_list_add_button(ctx->list, nullptr, nullptr);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<uintptr_t>(task.id)));
    lv_obj_add_event_cb(row, onTaskPressed, LV_EVENT_SHORT_CLICKED, ctx);
    lv_obj_add_event_cb(row, onTaskLongPressed, LV_EVENT_LONG_PRESSED, ctx);
    lv_obj_add_event_cb(row, onTaskFocused, LV_EVENT_FOCUSED, ctx);

    const int32_t size = lvgl_get_text_font_height(FONT_SIZE_DEFAULT) + 6;
    auto* check = lv_obj_create(row);
    lv_obj_remove_style_all(check);
    lv_obj_set_size(check, size, size);
    lv_obj_set_style_radius(check, 4, 0);
    lv_obj_set_style_border_width(check, 2, 0);
    lv_obj_set_style_border_color(check, lv_obj_get_style_text_color(row, LV_PART_MAIN), 0);
    lv_obj_set_style_border_opa(check, LV_OPA_70, 0);
    lv_obj_set_scrollable(check, false);
    lv_obj_set_clickable(check, true);
    lv_obj_set_user_data(check, reinterpret_cast<void*>(static_cast<uintptr_t>(task.id)));
    lv_obj_add_event_cb(check, onCheckPressed, LV_EVENT_SHORT_CLICKED, ctx);
    if (task.done) {
        lv_obj_center(createLabel(check, LV_SYMBOL_OK));
    }

    auto* title = createLabel(row, task.title.c_str());
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(title, lv_font_get_line_height(lv_obj_get_style_text_font(title, LV_PART_MAIN)));
    lv_obj_set_width(title, 0);
    lv_obj_set_flex_grow(title, 1);
    if (task.done) {
        lv_obj_set_style_text_decor(title, LV_TEXT_DECOR_STRIKETHROUGH, 0);
        lv_obj_set_style_text_opa(title, LV_OPA_50, 0);
    }

    if (!task.details.empty()) {
        lv_obj_set_style_text_opa(createLabel(row, LV_SYMBOL_LIST), LV_OPA_50, 0);
    }
}

void createListView(Context* ctx) {
    lvgl_toolbar_set_title(ctx->toolbar, "To-do");
    lvgl_toolbar_set_nav_action(ctx->toolbar, LV_SYMBOL_CLOSE, onExit, ctx);
    auto* overflow = lvgl_toolbar_add_text_button_action(ctx->toolbar, LVGL_ICON_SHARED_MORE_VERT, onOverflow, ctx);
    lv_obj_set_style_text_font(overflow, lvgl_get_shared_icon_default_font(), 0);

    // A tab per group, and one to add a group
    ctx->tabs = lvgl_tabs_create(ctx->wrapper);
    lvgl_tabs_set_mode(ctx->tabs, LVGL_TABS_MODE_SCROLLABLE);
    for (size_t i = 0; i < ctx->groups.size(); i++) {
        auto* tab = lvgl_tabs_add_tab(ctx->tabs, ctx->groups[i].c_str());
        lv_obj_set_user_data(tab, reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        lv_obj_add_event_cb(tab, onTabLongPressed, LV_EVENT_LONG_PRESSED, ctx);
    }
    lvgl_tabs_add_tab(ctx->tabs, "+");
    lvgl_tabs_set_selected(ctx->tabs, static_cast<uint32_t>(ctx->group));
    lv_obj_add_event_cb(ctx->tabs, onTabChanged, LV_EVENT_VALUE_CHANGED, ctx);

    ctx->list = lvgl_list_create(ctx->wrapper);
    lv_obj_set_width(ctx->list, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->list, 1);
    lv_obj_add_event_cb(ctx->list, onListKey, LV_EVENT_KEY, ctx);

    size_t shown = 0;
    for (const auto& task : ctx->tasks) {
        if (ctx->hideDone && task.done) continue;
        createRow(ctx, task);
        shown++;
    }
    if (ctx->storageFailed) {
        lvgl_list_add_text(ctx->list, "Storage error: changes may not be saved");
    } else if (shown == 0) {
        lvgl_list_add_text(ctx->list, ctx->tasks.empty() ? "No tasks in this group" : "All done");
    }
    lvgl_list_hide_last_divider(ctx->list);
    // For the arrow keys
    lvgl_grid_navigation_add(ctx->list);

    auto* inputRow = createPlain(ctx->wrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(inputRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(inputRow, 4, 0);
    lv_obj_set_style_pad_column(inputRow, 4, 0);

    ctx->input = lv_textarea_create(inputRow);
    lv_textarea_set_one_line(ctx->input, true);
    lv_textarea_set_max_length(ctx->input, MAX_TITLE_LENGTH);
    lv_textarea_set_placeholder_text(ctx->input, "New task...");
    lv_textarea_set_text(ctx->input, ctx->draft.c_str());
    lv_obj_set_flex_grow(ctx->input, 1);
    lv_obj_add_event_cb(ctx->input, onAdd, LV_EVENT_READY, ctx);

    auto* add = lv_button_create(inputRow);
    createLabel(add, LV_SYMBOL_PLUS);
    lv_obj_add_event_cb(add, onAdd, LV_EVENT_SHORT_CLICKED, ctx);
}

void createDetailView(Context* ctx, const Task& task) {
    lvgl_toolbar_set_title(ctx->toolbar, "Task");
    lvgl_toolbar_set_nav_action(ctx->toolbar, LV_SYMBOL_LEFT, onBack, ctx);
    lvgl_toolbar_add_text_button_action(ctx->toolbar, task.done ? "Reopen" : LV_SYMBOL_OK " Done", onDoneButton, ctx);

    ctx->titleInput = lv_textarea_create(ctx->wrapper);
    lv_textarea_set_one_line(ctx->titleInput, true);
    lv_textarea_set_max_length(ctx->titleInput, MAX_TITLE_LENGTH);
    lv_textarea_set_placeholder_text(ctx->titleInput, "Title");
    lv_textarea_set_text(ctx->titleInput, task.title.c_str());
    lv_obj_set_width(ctx->titleInput, LV_PCT(100));

    ctx->detailsInput = lv_textarea_create(ctx->wrapper);
    lv_textarea_set_max_length(ctx->detailsInput, MAX_DETAILS_LENGTH);
    lv_textarea_set_placeholder_text(ctx->detailsInput, "Details...");
    lv_textarea_set_text(ctx->detailsInput, task.details.c_str());
    lv_obj_set_width(ctx->detailsInput, LV_PCT(100));
    lv_obj_set_flex_grow(ctx->detailsInput, 1);
}

/** Keeps what was typed, before the widgets go away */
void keepInput(Context* ctx) {
    if (ctx->input != nullptr) ctx->draft = lv_textarea_get_text(ctx->input);
    saveOpenTask(ctx);
}

/** Rebuilds the view. Call keepInput() first, while the widgets still match the state. */
void render(Context* ctx) {
    if (ctx->wrapper == nullptr) return;

    const int32_t scroll = ctx->list != nullptr ? lv_obj_get_scroll_y(ctx->list) : 0;
    ctx->tabs = nullptr;
    ctx->list = nullptr;
    ctx->input = nullptr;
    ctx->titleInput = nullptr;
    ctx->detailsInput = nullptr;
    lv_obj_clean(ctx->wrapper);
    lvgl_toolbar_clear_actions(ctx->toolbar);

    const Task* task = findTask(ctx, ctx->openTask);
    if (task != nullptr) {
        createDetailView(ctx, *task);
    } else {
        ctx->openTask = 0;
        createListView(ctx);
        lv_obj_update_layout(ctx->list);
        lv_obj_scroll_to_y(ctx->list, scroll, LV_ANIM_OFF);
    }
}

/* ── Carrying out actions ─────────────────────────────────────────── */

void selectGroup(Context* ctx, size_t index) {
    ctx->group = index;
    ctx->tasks = ctx->store.loadTasks(currentGroup(ctx));
    ctx->focusedTask = 0;
    if (ctx->list != nullptr) lv_obj_scroll_to_y(ctx->list, 0, LV_ANIM_OFF);
    saveSettings(ctx);
}

void toggleTask(Context* ctx, uint32_t id) {
    Task* task = findTask(ctx, id);
    if (task == nullptr) return;
    task->done = !task->done;
    const time_t now = time(nullptr);
    task->completed = task->done ? static_cast<long long>(now) : 0;
    saveTask(ctx, *task);
}

void onDialogChoice(Context* ctx, int32_t button) {
    const Dialog dialog = ctx->dialog;
    ctx->dialog = Dialog::None;
    if (button < 0) return;

    switch (dialog) {
        case Dialog::Overflow:
            if (button == 0) {
                ctx->hideDone = !ctx->hideDone;
                saveSettings(ctx);
            } else if (button == 1) {
                showConfirm(ctx, Dialog::ClearDone, "Clear done", "Delete all finished tasks in this group?", "Delete");
            } else {
                showDeleteGroupConfirm(ctx);
            }
            break;
        case Dialog::ClearDone:
            if (button == 0) {
                for (const auto& task : ctx->tasks) {
                    if (task.done) ctx->store.deleteTask(currentGroup(ctx), task.id);
                }
                ctx->tasks = ctx->store.loadTasks(currentGroup(ctx));
            }
            break;
        case Dialog::TaskMenu:
            if (button == 0 && ctx->groups.size() > 1) {
                ctx->dialogGroups.clear();
                for (const auto& group : ctx->groups) {
                    if (group != currentGroup(ctx)) ctx->dialogGroups.push_back(group);
                }
                std::vector<const char*> items;
                for (const auto& group : ctx->dialogGroups) items.push_back(group.c_str());
                // With one other group, the dialog picks it right away
                showSelection(ctx, Dialog::MoveTask, "Move to", items);
            } else {
                queue(ctx, ActionType::ConfirmDeleteTask, ctx->targetTask);
            }
            break;
        case Dialog::MoveTask:
            if (static_cast<size_t>(button) < ctx->dialogGroups.size()) {
                if (!ctx->store.moveTask(currentGroup(ctx), ctx->dialogGroups[button], ctx->targetTask)) ctx->storageFailed = true;
                ctx->tasks = ctx->store.loadTasks(currentGroup(ctx));
            }
            break;
        case Dialog::DeleteTask:
            if (button == 0) {
                if (!ctx->store.deleteTask(currentGroup(ctx), ctx->targetTask)) ctx->storageFailed = true;
                ctx->tasks = ctx->store.loadTasks(currentGroup(ctx));
                ctx->focusedTask = 0;
            }
            break;
        case Dialog::GroupMenu:
            if (button == 0) {
                showNameDialog(ctx, Dialog::RenameGroup, "Rename group", currentGroup(ctx).c_str());
            } else {
                showDeleteGroupConfirm(ctx);
            }
            break;
        case Dialog::DeleteGroup:
            if (button == 0) {
                if (!ctx->store.deleteGroup(currentGroup(ctx))) ctx->storageFailed = true;
                reload(ctx, "");
                saveSettings(ctx);
            }
            break;
        default:
            break;
    }
}

void onDialogName(Context* ctx, const std::string& text) {
    const Dialog dialog = ctx->dialog;
    ctx->dialog = Dialog::None;
    const std::string name = TodoStore::cleanGroupName(text);
    if (name.empty() || name == "+") return;

    if (dialog == Dialog::NewGroup) {
        // An existing name selects that group
        ctx->store.createGroup(name);
        reload(ctx, name);
        saveSettings(ctx);
    } else if (dialog == Dialog::RenameGroup && name != currentGroup(ctx)) {
        const bool renamed = ctx->store.renameGroup(currentGroup(ctx), name);
        reload(ctx, renamed ? name : currentGroup(ctx));
        saveSettings(ctx);
    }
}

void run(Context* ctx, const Action& action) {
    switch (action.type) {
        case ActionType::SelectTab:
            if (action.value < ctx->groups.size()) {
                selectGroup(ctx, action.value);
            } else {
                // The "+" tab: render() selects the current group again
                showNameDialog(ctx, Dialog::NewGroup, "New group", "");
            }
            break;
        case ActionType::AddTask: {
            Task task;
            task.title = action.text.substr(action.text.find_first_not_of(' '));
            while (!task.title.empty() && task.title.back() == ' ') task.title.pop_back();
            saveTask(ctx, task);
            ctx->draft.clear();
            ctx->tasks = ctx->store.loadTasks(currentGroup(ctx));
            break;
        }
        case ActionType::ToggleTask:
            toggleTask(ctx, action.value);
            break;
        case ActionType::OpenTask:
            ctx->openTask = action.value;
            break;
        case ActionType::CloseTask:
            ctx->openTask = 0;
            break;
        case ActionType::ShowOverflow: {
            std::vector<const char*> items = { ctx->hideDone ? "Show done" : "Hide done", "Clear done" };
            // The only group can't be deleted
            if (ctx->groups.size() > 1) items.push_back("Delete group");
            showSelection(ctx, Dialog::Overflow, "To-do", items);
            break;
        }
        case ActionType::ShowTaskMenu:
            ctx->targetTask = action.value;
            if (ctx->groups.size() > 1) {
                showSelection(ctx, Dialog::TaskMenu, "Task", { "Move to group", "Delete" });
            } else {
                // A dialog with one item would pick it without asking
                queue(ctx, ActionType::ConfirmDeleteTask, action.value);
            }
            break;
        case ActionType::ConfirmDeleteTask:
            if (const Task* task = findTask(ctx, action.value)) {
                ctx->targetTask = action.value;
                showConfirm(ctx, Dialog::DeleteTask, "Delete task", "Delete \"" + task->title + "\"?", "Delete");
            }
            break;
        case ActionType::ShowGroupMenu:
            if (action.value < ctx->groups.size()) {
                selectGroup(ctx, action.value);
                showSelection(ctx, Dialog::GroupMenu, currentGroup(ctx).c_str(), { "Rename", "Delete" });
            }
            break;
        case ActionType::DialogResult:
            onDialogChoice(ctx, static_cast<int32_t>(action.value));
            break;
        case ActionType::DialogText:
            onDialogName(ctx, action.text);
            break;
        case ActionType::Refresh:
            break;
    }
}

} // namespace

/* ── Lifecycle ────────────────────────────────────────────────────── */

void todoListInit(Context* ctx) {
    ctx->storageFailed = !ctx->store.init();

    char group[64] = {};
    if (!ctx->storageFailed) {
        if (Preferences* preferences = preferences_open(ctx->store.getSettingsPath().c_str())) {
            preferences_opt_string(preferences, "group", group, sizeof(group));
            preferences_opt_bool(preferences, "hide_done", &ctx->hideDone);
            preferences_close(preferences);
        }
    }
    reload(ctx, group);
}

void todoListRunActions(Context* ctx) {
    lvgl_lock();
    // Actions can queue more actions
    while (!ctx->actions.empty()) {
        const Action action = ctx->actions.front();
        ctx->actions.erase(ctx->actions.begin());
        keepInput(ctx);
        run(ctx, action);
        render(ctx);
    }
    lvgl_unlock();
}

void todoListCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);

    ctx->toolbar = lvgl_toolbar_create(parent, "To-do");

    ctx->wrapper = createPlain(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(ctx->wrapper, 1);

    // Rendering needs the widgets above, and happens on the app task like all other rendering
    queue(ctx, ActionType::Refresh);
}

void todoListDestroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    keepInput(ctx);
    ctx->toolbar = nullptr;
    ctx->wrapper = nullptr;
    ctx->tabs = nullptr;
    ctx->list = nullptr;
    ctx->input = nullptr;
    ctx->titleInput = nullptr;
    ctx->detailsInput = nullptr;
}
