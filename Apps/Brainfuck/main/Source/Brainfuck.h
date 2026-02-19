#pragma once

#include <tt_app.h>
#include <lvgl.h>
#include <TactilityCpp/App.h>

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

class Brainfuck final : public App {

private:
    // UI pointers (nulled in onHide)
    lv_obj_t* outputTa = nullptr;
    lv_obj_t* inputTa = nullptr;
    lv_obj_t* inputRow = nullptr;
    lv_obj_t* examplesList = nullptr;
    lv_obj_t* clrBtn = nullptr;

    BfState state = BfState::Examples;
    BfVM vm = {};

    // Helper methods
    void bfInit();
    void bfRun(const char* code);
    void runCode(const char* code);
    void buildScriptList(lv_obj_t* list);

    // View management
    void showMainView();
    void showExamplesView();

    // Static callbacks
    static void onRunClicked(lv_event_t* e);
    static void onClearClicked(lv_event_t* e);
    static void onExamplesClicked(lv_event_t* e);
    static void onExampleSelected(lv_event_t* e);
    static void onFileSelected(lv_event_t* e);
    static void onInputReady(lv_event_t* e);

public:
    void onShow(AppHandle context, lv_obj_t* parent) override;
    void onHide(AppHandle context) override;
};
