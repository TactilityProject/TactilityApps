#pragma once

#include "TodoStore.h"

#include <app/instance.h>
#include <tactility/concurrent/task_event_group.h>

#include <lvgl.h>

#include <atomic>
#include <string>
#include <vector>

/** What the user did. Widgets and dialogs queue these, and the app task carries them out. */
enum class ActionType {
    SelectTab,
    AddTask,
    ToggleTask,
    OpenTask,
    CloseTask,
    ShowOverflow,
    ShowTaskMenu,
    ConfirmDeleteTask,
    ShowGroupMenu,
    Refresh,
    DialogResult,
    DialogText,
};

struct Action {
    ActionType type;
    uint32_t value = 0; // a task id, tab index or dialog button
    std::string text;
};

/** The dialog that is shown, if any */
enum class Dialog {
    None,
    Overflow,
    TaskMenu,
    MoveTask,
    DeleteTask,
    GroupMenu,
    DeleteGroup,
    ClearDone,
    NewGroup,
    RenameGroup,
};

struct Context {
    AppInstanceId appInstanceId = 0;

    // Everything below is guarded by the LVGL lock
    TodoStore store;
    bool storageFailed = false;
    std::vector<std::string> groups;
    size_t group = 0;
    std::vector<Task> tasks; // of the selected group
    bool hideDone = false;
    uint32_t openTask = 0;   // the id of the task on the detail page, 0 for the list
    uint32_t targetTask = 0; // the id of the task that a dialog is about
    uint32_t focusedTask = 0;
    std::string draft;       // the text of the quick add field
    std::vector<std::string> dialogGroups; // the groups that "Move to" lists
    Dialog dialog = Dialog::None;
    std::vector<Action> actions;

    TaskEventGroup* eventGroup = nullptr;
    uint32_t actionBit = 0;

    // UI pointers (nulled when the widgets are destroyed)
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* wrapper = nullptr;
    lv_obj_t* tabs = nullptr;
    lv_obj_t* list = nullptr;
    lv_obj_t* input = nullptr;
    lv_obj_t* titleInput = nullptr;
    lv_obj_t* detailsInput = nullptr;
};

/** Loads the tasks. Call before the window is created. */
void todoListInit(Context* ctx);

/** window_manager_create_ext()'s callbacks - @a userData is the Context* for this instance. */
void todoListCreateWidgets(lv_obj_t* parent, void* userData);
void todoListDestroyWidgets(void* userData);

/** Carries out the queued actions. Call on the app task, without the LVGL lock. */
void todoListRunActions(Context* ctx);
