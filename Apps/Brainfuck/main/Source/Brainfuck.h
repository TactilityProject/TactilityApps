#pragma once

#include <lvgl.h>
#include <stdint.h>

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

enum class BfState {
    Main,
    Examples,
};

struct Context {
    uint32_t appInstanceId;

    BfState state = BfState::Examples;
    BfVM vm = {};

    // UI pointers (nulled on teardown)
    lv_obj_t* outputTa = nullptr;
    lv_obj_t* inputTa = nullptr;
    lv_obj_t* inputRow = nullptr;
    lv_obj_t* examplesList = nullptr;
    lv_obj_t* clrBtn = nullptr;

    char** scriptPaths = nullptr;
    int scriptCount = 0;
};

/** window_manager_create()'s WindowCreateWidgetsFn - @a userData is the Context* for this instance. */
void brainfuckCreateWidgets(lv_obj_t* parent, void* userData);

/** Releases resources acquired while the window was shown (script path list). Call once the window is torn down. */
void brainfuckTeardown(Context* ctx);
