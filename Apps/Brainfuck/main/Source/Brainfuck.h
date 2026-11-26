#pragma once

#include <app/instance.h>
#include <app/stream.h>
#include <tactility/concurrent/task_event_group.h>

#include <lvgl.h>
#include <stdint.h>

#include <atomic>
#include <string>
#include <vector>

constexpr int TAPE_SIZE = 4096;
constexpr int MAX_OUTPUT = 2048;
constexpr int MAX_CYCLES = 2000000;

struct BfVM {
    uint8_t tape[TAPE_SIZE];
    int ptr;
    int pc;
    int cycles;
    char output[MAX_OUTPUT];
    int outLen;
    bool error;
    char errorMsg[64];
};

/** The items of the overflow menu */
enum class BfAction { New, Open, Save, SaveAs, Examples };

/** The dialog that is shown, if any */
enum class BfDialog { None, Menu, Examples };

struct Context {
    AppInstanceId appInstanceId = 0;
    BfVM vm = {};

    // The editor's text while the window has no widgets
    std::string code;
    /** The file that is being edited, empty for an unsaved one */
    std::string filePath;

    // A dialog's result is handled on the app task, which dialogBit wakes
    TaskEventGroup* eventGroup = nullptr;
    uint32_t dialogBit = 0;
    std::atomic<BfDialog> dialog { BfDialog::None };
    std::atomic<int32_t> dialogResult { -1 };
    /** The actions of the items of the menu that is shown */
    std::vector<BfAction> menuActions;

    // The file selection app, while it runs
    uint32_t fileLaunchId = 0;
    bool fileSaving = false;
    AppStream fileStream {};
    uint8_t fileBuffer[256] {};

    // UI pointers (nulled when the widgets are destroyed)
    lv_obj_t* toolbar = nullptr;
    lv_obj_t* inputTa = nullptr;
    lv_obj_t* outputTa = nullptr;
    lv_obj_t* statusline = nullptr;
};

/** window_manager_create_ext()'s callbacks - @a userData is the Context* for this instance. */
void brainfuckCreateWidgets(lv_obj_t* parent, void* userData);
void brainfuckDestroyWidgets(void* userData);

// Call on the app task, without the LVGL lock

void brainfuckDialogResult(Context* ctx);
void brainfuckFileResult(Context* ctx, uint32_t launchId, int32_t result);
