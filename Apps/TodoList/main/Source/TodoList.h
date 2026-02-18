#pragma once

#include <tt_app.h>
#include <lvgl.h>
#include <TactilityCpp/App.h>

class TodoList final : public App {

private:

    static constexpr int MAX_TODOS = 50;
    static constexpr int MAX_TEXT_LEN = 128;

    struct TodoItem {
        char text[MAX_TEXT_LEN];
        bool done;
    };

    // UI pointers (nulled in onHide)
    lv_obj_t* list = nullptr;
    lv_obj_t* inputRow = nullptr;
    lv_obj_t* inputTa = nullptr;
    lv_obj_t* countLabel = nullptr;

    // Data
    TodoItem items[MAX_TODOS] = {};
    int count = 0;

    // Persistence
    void saveTodos();
    void loadTodos();

    // UI helpers
    void updateCountLabel();
    void rebuildList();
    void addItem(const char* text);

    // Static callbacks
    static void onItemClicked(lv_event_t* e);
    static void onDeleteClicked(lv_event_t* e);
    static void onAddClicked(lv_event_t* e);
    static void onInputReady(lv_event_t* e);
    static void onClearDoneClicked(lv_event_t* e);

public:
    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
};
