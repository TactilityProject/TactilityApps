#include "Brainfuck.h"
#include <tt_app.h>
#include <tt_lvgl_toolbar.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Built-in examples ────────────────────────────────────────────── */

struct BfExample {
    const char* name;
    const char* code;
};

static const BfExample examples[] = {
    {
        "Hello World (built-in)",
        "++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]"
        ">>.>---.+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++."
    },
    {
        "Fibonacci (built-in)",
        ">++++++++++>+>+[[+++++[>++++++++<-]>.<++++++[>--------<-]+<<<"
        "]>.>>[[-]<[>+<-]>>[<<+>+>-]<[>+<-[>+<-[>+<-[>+<-[>+<-[>+<-"
        "[>+<-[>+<-[>+<-[>[-]>+>+<<<-[>+<-]]]]]]]]]]]+>>>]<<<]"
    },
    {
        "Alphabet (built-in)",
        "+++++[>+++++++++++++<-]++++++++++++++++++++++++++[>.+<-]"
    },
    {
        "Beer (built-in)",
        ">++++++++++[<++++++++++>-]<->>>>>+++[>+++>+++<<-]<<<<+<[>[>+"
        ">+<<-]>>[-<<+>>]++++>+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<<[[-]>>"
        ">>>>[[-]<++++++++++<->>]<-[>+>+<<-]>[<+>-]+>[[-]<->]<<<<<<<<"
        "<->>]<[>+>+<<-]>>[-<<+>>]+>+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<<"
        "<[>>+>+<<<-]>>>[-<<<+>>>]++>+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<"
        "<[>+<[-]]<[>>+<<[-]]>>[<<+>>[-]]<<<[>>+>+<<<-]>>>[-<<<+>>>]+"
        "+++>+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<<[>+<[-]]<[>>+<<[-]]>>[<"
        "<+>>[-]]<<[[-]>>>++++++++[>>++++++<<-]>[<++++++++[>++++++<-]"
        ">.<++++++++[>------<-]>[<<+>>-]]>.<<++++++++[>>------<<-]<[-"
        ">>+<<]<++++++++[<++++>-]<.>+++++++[>+++++++++<-]>+++.<+++++["
        ">+++++++++<-]>.+++++..--------.-------.++++++++++++++>>[>>>+"
        ">+<<<<-]>>>>[-<<<<+>>>>]>+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<<<<"
        "[>>>+>+<<<<-]>>>>[-<<<<+>>>>]+>+<[-<->]<[[-]>>-<<]>>[[-]<<+>"
        ">]<<<[>>+<<[-]]>[>+<[-]]++>>+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<"
        "+<[[-]>-<]>[<<<<<<<.>>>>>>>[-]]<<<<<<<<<.>>----.---------.<<"
        ".>>----.+++..+++++++++++++.[-]<<[-]]<[>+>+<<-]>>[-<<+>>]+>+<"
        "[-<->]<[[-]>>-<<]>>[[-]<<+>>]<<<[>>+>+<<<-]>>>[-<<<+>>>]++++"
        ">+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<<[>+<[-]]<[>>+<<[-]]>>[<<+>"
        ">[-]]<<[[-]>++++++++[<++++>-]<.>++++++++++[>+++++++++++<-]>+"
        ".-.<<.>>++++++.------------.---.<<.>++++++[>+++<-]>.<++++++["
        ">----<-]>++.+++++++++++..[-]<<[-]++++++++++.[-]]<[>+>+<<-]>>"
        "[-<<+>>]+++>+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<<[[-]++++++++++."
        ">+++++++++[>+++++++++<-]>+++.+++++++++++++.++++++++++.------"
        ".<++++++++[>>++++<<-]>>.<++++++++++.-.---------.>.<-.+++++++"
        "++++.++++++++.---------.>.<-------------.+++++++++++++.-----"
        "-----.>.<++++++++++++.---------------.<+++[>++++++<-]>..>.<-"
        "---------.+++++++++++.>.<<+++[>------<-]>-.+++++++++++++++++"
        ".---.++++++.-------.----------.[-]>[-]<<<.[-]]<[>+>+<<-]>>[-"
        "<<+>>]++++>+<[-<->]<[[-]>>-<<]>>[[-]<<+>>]<<[[-]++++++++++.["
        "-]<[-]>]<+<]"
    },
};

static constexpr int NUM_EXAMPLES = sizeof(examples) / sizeof(examples[0]);

/* ── App handle for user data path ────────────────────────────────── */

static AppHandle s_appHandle = nullptr;

static bool getScriptDir(char* buf, size_t bufSize) {
    if (!s_appHandle) return false;
    size_t size = bufSize;
    tt_app_get_user_data_path(s_appHandle, buf, &size);
    if (size == 0) return false;
    mkdir(buf, 0755);
    return true;
}

static char** scriptPaths = nullptr;
static int scriptCount = 0;

static int ciStrcmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : *a;
        int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : *b;
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void freeScriptPaths() {
    for (int i = 0; i < scriptCount; i++) {
        free(scriptPaths[i]);
    }
    free(scriptPaths);
    scriptPaths = nullptr;
    scriptCount = 0;
}

/* File-scope instance pointer for index-based and path-based callbacks */
static Brainfuck* g_instance = nullptr;

/* ── VM Logic ─────────────────────────────────────────────────────── */

void Brainfuck::bfInit() {
    memset(&vm, 0, sizeof(BfVM));
}

static int bfFindBracket(const char* code, int pos, int dir) {
    int depth = 1;
    while (depth > 0) {
        pos += dir;
        if (pos < 0 || code[pos] == '\0') return -1;
        if (code[pos] == '[') depth += dir;
        if (code[pos] == ']') depth -= dir;
    }
    return pos;
}

void Brainfuck::bfRun(const char* code) {
    int len = strlen(code);

    while (vm.pc < len && !vm.error) {
        vm.cycles++;
        if (vm.cycles > MAX_CYCLES) {
            vm.error = true;
            snprintf(vm.errorMsg, sizeof(vm.errorMsg), "Halted after %d cycles (infinite loop?)", MAX_CYCLES);
            return;
        }

        char c = code[vm.pc];
        switch (c) {
            case '>':
                vm.ptr++;
                if (vm.ptr >= TAPE_SIZE) {
                    vm.error = true;
                    snprintf(vm.errorMsg, sizeof(vm.errorMsg), "Pointer overflow (>%d)", TAPE_SIZE);
                    return;
                }
                break;
            case '<':
                vm.ptr--;
                if (vm.ptr < 0) {
                    vm.error = true;
                    snprintf(vm.errorMsg, sizeof(vm.errorMsg), "Pointer underflow (<0)");
                    return;
                }
                break;
            case '+': vm.tape[vm.ptr]++; break;
            case '-': vm.tape[vm.ptr]--; break;
            case '.':
                if (vm.outLen < MAX_OUTPUT - 1) {
                    vm.output[vm.outLen++] = (char)vm.tape[vm.ptr];
                    vm.output[vm.outLen] = '\0';
                }
                break;
            case ',': vm.tape[vm.ptr] = 0; break;
            case '[':
                if (vm.tape[vm.ptr] == 0) {
                    int target = bfFindBracket(code, vm.pc, 1);
                    if (target < 0) {
                        vm.error = true;
                        snprintf(vm.errorMsg, sizeof(vm.errorMsg), "Unmatched '[' at pos %d", vm.pc);
                        return;
                    }
                    vm.pc = target;
                }
                break;
            case ']':
                if (vm.tape[vm.ptr] != 0) {
                    int target = bfFindBracket(code, vm.pc, -1);
                    if (target < 0) {
                        vm.error = true;
                        snprintf(vm.errorMsg, sizeof(vm.errorMsg), "Unmatched ']' at pos %d", vm.pc);
                        return;
                    }
                    vm.pc = target;
                }
                break;
        }
        vm.pc++;
    }
}

void Brainfuck::runCode(const char* code) {
    if (!outputTa) return;
    bfInit();
    bfRun(code);

    constexpr int resultSize = MAX_OUTPUT + 128;
    char* result = (char*)malloc(resultSize);
    if (!result) {
        lv_textarea_set_text(outputTa, "Out of memory");
        return;
    }
    int pos = 0;
    int remaining;

    remaining = resultSize - pos;
    if (remaining > 0) {
        int n = snprintf(result + pos, remaining, "> RUN (%d chars)\n", (int)strlen(code));
        pos += (n < remaining) ? n : (remaining - 1);
    }

    if (vm.outLen > 0) {
        remaining = resultSize - pos;
        if (remaining > 0) {
            int n = snprintf(result + pos, remaining, "%s\n", vm.output);
            pos += (n < remaining) ? n : (remaining - 1);
        }
    }

    if (vm.error) {
        remaining = resultSize - pos;
        if (remaining > 0) {
            int n = snprintf(result + pos, remaining, "ERROR: %s\n", vm.errorMsg);
            pos += (n < remaining) ? n : (remaining - 1);
        }
    } else {
        remaining = resultSize - pos;
        if (remaining > 0) {
            int n = snprintf(result + pos, remaining, "OK (%d cycles)\n", vm.cycles);
            pos += (n < remaining) ? n : (remaining - 1);
        }
    }

    lv_textarea_set_text(outputTa, result);
    lv_obj_scroll_to_y(outputTa, LV_COORD_MAX, LV_ANIM_ON);
    free(result);
}

/* ── View management ──────────────────────────────────────────────── */

void Brainfuck::showMainView() {
    state = BfState::Main;
    if (examplesList) lv_obj_add_flag(examplesList, LV_OBJ_FLAG_HIDDEN);
    if (outputTa) lv_obj_remove_flag(outputTa, LV_OBJ_FLAG_HIDDEN);
    if (inputRow) lv_obj_remove_flag(inputRow, LV_OBJ_FLAG_HIDDEN);
    if (clrBtn) lv_obj_remove_flag(clrBtn, LV_OBJ_FLAG_HIDDEN);
}

void Brainfuck::showExamplesView() {
    state = BfState::Examples;
    if (outputTa) lv_obj_add_flag(outputTa, LV_OBJ_FLAG_HIDDEN);
    if (inputRow) lv_obj_add_flag(inputRow, LV_OBJ_FLAG_HIDDEN);
    if (examplesList) lv_obj_remove_flag(examplesList, LV_OBJ_FLAG_HIDDEN);
    if (clrBtn) lv_obj_add_flag(clrBtn, LV_OBJ_FLAG_HIDDEN);
}

/* ── Callbacks ────────────────────────────────────────────────────── */

void Brainfuck::onRunClicked(lv_event_t* e) {
    if (!g_instance || !g_instance->inputTa) return;
    const char* code = lv_textarea_get_text(g_instance->inputTa);
    if (code && code[0]) {
        g_instance->runCode(code);
    }
}

void Brainfuck::onClearClicked(lv_event_t* e) {
    if (!g_instance) return;
    if (g_instance->outputTa) lv_textarea_set_text(g_instance->outputTa, "");
    if (g_instance->inputTa) lv_textarea_set_text(g_instance->inputTa, "");
}

void Brainfuck::onExamplesClicked(lv_event_t* e) {
    if (!g_instance) return;
    if (g_instance->state == BfState::Examples) {
        g_instance->showMainView();
    } else {
        g_instance->showExamplesView();
    }
}

void Brainfuck::onExampleSelected(lv_event_t* e) {
    if (!g_instance) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < NUM_EXAMPLES) {
        if (g_instance->inputTa) lv_textarea_set_text(g_instance->inputTa, examples[idx].code);
        g_instance->showMainView();
        g_instance->runCode(examples[idx].code);
    }
}

void Brainfuck::onFileSelected(lv_event_t* e) {
    if (!g_instance) return;
    const char* path = (const char*)lv_event_get_user_data(e);
    FILE* f = fopen(path, "rb");
    if (!f) {
        if (g_instance->outputTa) lv_textarea_set_text(g_instance->outputTa, "Cannot open file");
        g_instance->showMainView();
        return;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (fsize <= 0 || fsize > 32768) {
        fclose(f);
        if (g_instance->outputTa) lv_textarea_set_text(g_instance->outputTa, "File too large or empty");
        g_instance->showMainView();
        return;
    }
    fseek(f, 0, SEEK_SET);

    char* buf = (char*)malloc(fsize + 1);
    if (!buf) {
        fclose(f);
        if (g_instance->outputTa) lv_textarea_set_text(g_instance->outputTa, "Out of memory");
        g_instance->showMainView();
        return;
    }

    size_t bytesRead = fread(buf, 1, fsize, f);
    fclose(f);
    buf[bytesRead] = '\0';

    if (g_instance->inputTa) lv_textarea_set_text(g_instance->inputTa, buf);
    g_instance->showMainView();
    g_instance->runCode(buf);
    free(buf);
}

void Brainfuck::onInputReady(lv_event_t* e) {
    onRunClicked(e);
}

/* ── Script list building ─────────────────────────────────────────── */

void Brainfuck::buildScriptList(lv_obj_t* list) {
    freeScriptPaths();

    for (int i = 0; i < NUM_EXAMPLES; i++) {
        lv_obj_t* btn = lv_list_add_button(list, LV_SYMBOL_PLAY, examples[i].name);
        lv_obj_add_event_cb(btn, onExampleSelected, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }

    char scriptDir[256];
    if (!getScriptDir(scriptDir, sizeof(scriptDir))) {
        lv_list_add_text(list, "No storage available for .bf files");
        return;
    }

    DIR* dir = opendir(scriptDir);
    if (!dir) {
        char hint[300];
        snprintf(hint, sizeof(hint), "Put .bf files in %s", scriptDir);
        lv_list_add_text(list, hint);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len < 3) continue;

        const char* ext3 = &name[len - 3];
        const char* ext2 = &name[len - 2];
        if (ciStrcmp(ext3, ".bf") != 0 && ciStrcmp(ext2, ".b") != 0) {
            continue;
        }

        size_t pathLen = strlen(scriptDir) + 1 + len + 1;
        char* path = (char*)malloc(pathLen);
        if (!path) break;
        snprintf(path, pathLen, "%s/%s", scriptDir, name);

        char** tmp = (char**)realloc(scriptPaths, sizeof(char*) * (scriptCount + 1));
        if (!tmp) { free(path); break; }
        scriptPaths = tmp;
        scriptPaths[scriptCount] = path;

        lv_obj_t* btn = lv_list_add_button(list, LV_SYMBOL_FILE, name);
        lv_obj_add_event_cb(btn, onFileSelected, LV_EVENT_CLICKED, path);
        scriptCount++;
    }
    closedir(dir);

    if (scriptCount == 0) {
        lv_list_add_text(list, "No custom scripts found on storage");
    }
}

/* ── Lifecycle ────────────────────────────────────────────────────── */

void Brainfuck::onShow(AppHandle app, lv_obj_t* parent) {
    g_instance = this;
    s_appHandle = app;
    state = BfState::Examples;

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* toolbar = tt_lvgl_toolbar_create_for_app(parent, app);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);
    clrBtn = tt_lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_TRASH, onClearClicked, nullptr);
    lv_obj_add_flag(clrBtn, LV_OBJ_FLAG_HIDDEN);
    tt_lvgl_toolbar_add_text_button_action(toolbar, LV_SYMBOL_LIST, onExamplesClicked, nullptr);

    lv_obj_t* cont = lv_obj_create(parent);
    lv_obj_set_width(cont, LV_PCT(100));
    lv_obj_set_flex_grow(cont, 1);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_set_style_pad_gap(cont, 4, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(cont, 0, 0);

    outputTa = lv_textarea_create(cont);
    lv_textarea_set_text(outputTa, "");
    lv_textarea_set_cursor_click_pos(outputTa, false);
    lv_obj_add_state(outputTa, LV_STATE_DISABLED);
    lv_obj_set_width(outputTa, LV_PCT(100));
    lv_obj_set_flex_grow(outputTa, 1);
    lv_obj_set_style_text_font(outputTa, lv_font_get_default(), 0);
    lv_obj_add_flag(outputTa, LV_OBJ_FLAG_HIDDEN);

    inputRow = lv_obj_create(cont);
    lv_obj_set_size(inputRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(inputRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(inputRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(inputRow, 0, 0);
    lv_obj_set_style_pad_gap(inputRow, 4, 0);
    lv_obj_remove_flag(inputRow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(inputRow, 0, 0);
    lv_obj_add_flag(inputRow, LV_OBJ_FLAG_HIDDEN);

    inputTa = lv_textarea_create(inputRow);
    lv_textarea_set_placeholder_text(inputTa, "++++++[>++++++++++<-]>+++++.");
    lv_textarea_set_one_line(inputTa, false);
    lv_obj_set_flex_grow(inputTa, 1);
    lv_obj_set_height(inputTa, 50);
    lv_obj_set_style_text_font(inputTa, lv_font_get_default(), 0);
    lv_obj_add_event_cb(inputTa, onInputReady, LV_EVENT_READY, nullptr);

    lv_obj_t* runBtn = lv_button_create(inputRow);
    lv_obj_t* runLbl = lv_label_create(runBtn);
    lv_label_set_text(runLbl, LV_SYMBOL_PLAY);
    lv_obj_add_event_cb(runBtn, onRunClicked, LV_EVENT_CLICKED, nullptr);

    examplesList = lv_list_create(cont);
    lv_obj_set_width(examplesList, LV_PCT(100));
    lv_obj_set_flex_grow(examplesList, 1);
    buildScriptList(examplesList);
}

void Brainfuck::onHide(AppHandle app) {
    freeScriptPaths();
    outputTa = nullptr;
    inputTa = nullptr;
    inputRow = nullptr;
    examplesList = nullptr;
    clrBtn = nullptr;
    g_instance = nullptr;
    s_appHandle = nullptr;
}
