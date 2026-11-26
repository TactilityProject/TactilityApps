#include "Brainfuck.h"

#include <app/paths.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/shared.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/statusline.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/dialog.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/** Must match manifest.properties' app.id */
static constexpr const char* APP_ID = "tactility.brainfuck";

/* ── Built-in examples ────────────────────────────────────────────── */

struct BfExample {
    const char* name;
    const char* code;
};

static const BfExample examples[] = {
    {
        "Hello World",
        "++++++++[>++++[>++>+++>+++>+<<<<-]>+>+>->>+[<]<-]"
        ">>.>---.+++++++..+++.>>.<-.<.+++.------.--------.>>+.>++."
    },
    {
        "Fibonacci",
        ">++++++++++>+>+[[+++++[>++++++++<-]>.<++++++[>--------<-]+<<<"
        "]>.>>[[-]<[>+<-]>>[<<+>+>-]<[>+<-[>+<-[>+<-[>+<-[>+<-[>+<-"
        "[>+<-[>+<-[>+<-[>[-]>+>+<<<-[>+<-]]]]]]]]]]]+>>>]<<<]"
    },
    {
        "Alphabet",
        "+++++[>+++++++++++++<-]++++++++++++++++++++++++++[>.+<-]"
    },
    {
        "Beer",
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
    // Unused last entry. The ELF loader doesn't relocate a pointer to one past the end of a section,
    // which is what the end of a loop over this table would be without it.
    { nullptr, nullptr },
};

static constexpr int NUM_EXAMPLES = sizeof(examples) / sizeof(examples[0]) - 1;

/* ── Files ────────────────────────────────────────────────────────── */

static constexpr long MAX_SCRIPT_SIZE = 32768;

/** The directory that the file selection starts in, which is created when needed */
static bool getScriptDir(char* buf, size_t bufSize) {
    if (app_paths_get_user_data_directory(APP_ID, buf, bufSize) != ERROR_NONE) {
        return false;
    }
    for (char* p = buf + 1; *p; ++p) {
        if (*p == '/') { *p = '\0'; mkdir(buf, 0755); *p = '/'; }
    }
    mkdir(buf, 0755);
    return true;
}

/** @return false when the file can't be read or is too large */
static bool readScript(const std::string& path, std::string& code) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) return false;
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    bool success = size >= 0 && size <= MAX_SCRIPT_SIZE;
    if (success) {
        code.resize(size);
        success = fread(code.data(), 1, size, file) == static_cast<size_t>(size);
    }
    fclose(file);
    return success;
}

/** Writes a temporary file first, so the existing script survives a failure */
static bool writeScript(const std::string& path, const std::string& code) {
    const std::string temporary = path + ".tmp";
    FILE* file = fopen(temporary.c_str(), "wb");
    if (file == nullptr) return false;
    bool success = fwrite(code.data(), 1, code.size(), file) == code.size();
    success = (fclose(file) == 0) && success;
    if (success && rename(temporary.c_str(), path.c_str()) != 0) {
        // FAT can't rename onto an existing file, so move that out of the way
        const std::string backup = path + ".bak";
        unlink(backup.c_str());
        success = rename(path.c_str(), backup.c_str()) == 0;
        if (success) {
            success = rename(temporary.c_str(), path.c_str()) == 0;
            if (success) unlink(backup.c_str());
            else rename(backup.c_str(), path.c_str());
        }
    }
    if (!success) unlink(temporary.c_str());
    return success;
}

/* ── VM Logic ─────────────────────────────────────────────────────── */

static void bfInit(BfVM& vm) {
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

static void bfRun(BfVM& vm, const char* code) {
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

/* ── Running ──────────────────────────────────────────────────────── */

/** Requires the LVGL lock */
static void setStatus(Context* ctx, const char* text, LvglStatuslineLevel level) {
    if (ctx->statusline == nullptr) return;
    lvgl_statusline_set_text(ctx->statusline, text);
    lvgl_statusline_set_level(ctx->statusline, level);
    lv_obj_set_hidden(ctx->statusline, false);
}

/** Runs the code in the editor. Requires the LVGL lock. */
static void runCode(Context* ctx) {
    if (ctx->inputTa == nullptr || ctx->outputTa == nullptr) return;
    const char* code = lv_textarea_get_text(ctx->inputTa);
    if (code[0] == '\0') {
        setStatus(ctx, "Type some code first", LVGL_STATUSLINE_LEVEL_WARNING);
        return;
    }

    bfInit(ctx->vm);
    bfRun(ctx->vm, code);

    lv_textarea_set_text(ctx->outputTa, ctx->vm.output);
    lv_obj_scroll_to_y(ctx->outputTa, 0, LV_ANIM_OFF);

    if (ctx->vm.error) {
        setStatus(ctx, ctx->vm.errorMsg, LVGL_STATUSLINE_LEVEL_ERROR);
    } else {
        char status[64];
        snprintf(status, sizeof(status), "%s in %d cycles", ctx->vm.outLen > 0 ? "Finished" : "Finished without output", ctx->vm.cycles);
        setStatus(ctx, status, LVGL_STATUSLINE_LEVEL_OK);
    }
}

/* ── Document ─────────────────────────────────────────────────────── */

/** The toolbar shows the name of the file. Requires the LVGL lock. */
static void updateTitle(Context* ctx) {
    if (ctx->toolbar == nullptr) return;
    const size_t slash = ctx->filePath.find_last_of('/');
    const std::string title = ctx->filePath.empty() ? "Brainfuck" : ctx->filePath.substr(slash + 1);
    lvgl_toolbar_set_title(ctx->toolbar, title.c_str());
}

/** Replaces the code in the editor. The widgets may not exist, e.g. while the file selection covers the window. */
static void setDocument(Context* ctx, const std::string& code, const std::string& path) {
    lvgl_lock();
    ctx->code = code;
    ctx->filePath = path;
    ctx->vm.output[0] = '\0';
    if (ctx->inputTa != nullptr) {
        lv_textarea_set_text(ctx->inputTa, code.c_str());
        lv_textarea_set_text(ctx->outputTa, "");
        lv_obj_set_hidden(ctx->statusline, true);
        updateTitle(ctx);
    }
    lvgl_unlock();
}

static void showStatus(Context* ctx, const char* text, LvglStatuslineLevel level) {
    lvgl_lock();
    setStatus(ctx, text, level);
    lvgl_unlock();
}

static void save(Context* ctx, const std::string& path) {
    lvgl_lock();
    if (ctx->inputTa != nullptr) {
        ctx->code = lv_textarea_get_text(ctx->inputTa);
    }
    const std::string code = ctx->code;
    lvgl_unlock();

    if (code.size() > static_cast<size_t>(MAX_SCRIPT_SIZE)) {
        showStatus(ctx, "Too large to save (over 32 KB)", LVGL_STATUSLINE_LEVEL_ERROR);
        return;
    }

    if (!writeScript(path, code)) {
        showStatus(ctx, "Can't save the file", LVGL_STATUSLINE_LEVEL_ERROR);
        return;
    }

    lvgl_lock();
    ctx->filePath = path;
    updateTitle(ctx);
    setStatus(ctx, "Saved", LVGL_STATUSLINE_LEVEL_OK);
    lvgl_unlock();
}

/* ── Dialogs ──────────────────────────────────────────────────────── */

static void onDialogResult(int32_t index, void* userData) {
    // Handled on the app task, as the dialog task's stack is small
    auto* ctx = static_cast<Context*>(userData);
    ctx->dialogResult = index;
    task_event_group_signal(ctx->eventGroup, ctx->dialogBit);
}

static void showSelection(Context* ctx, BfDialog dialog, const char* title, const std::vector<const char*>& items, const std::vector<const char*>& icons) {
    ctx->dialog = dialog;
    if (window_manager_selection_dialog_with_icons(ctx->appInstanceId, title, items.data(), icons.data(),
            (uint32_t)items.size(), onDialogResult, ctx) != ERROR_NONE) {
        ctx->dialog = BfDialog::None;
    }
}

static void onRunClicked(lv_event_t* e) {
    runCode(static_cast<Context*>(lv_event_get_user_data(e)));
}

static void onMenuClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    std::vector<const char*> items;
    std::vector<const char*> icons;
    ctx->menuActions.clear();
    auto add = [&](BfAction action, const char* label, const char* icon) {
        ctx->menuActions.push_back(action);
        items.push_back(label);
        icons.push_back(icon);
    };
    add(BfAction::New, "New", LVGL_ICON_SHARED_NOTE_ADD);
    add(BfAction::Open, "Open", LVGL_ICON_SHARED_FOLDER_OPEN);
    // Saving needs a file, which "Save as" picks
    if (!ctx->filePath.empty()) {
        add(BfAction::Save, "Save", LVGL_ICON_SHARED_SAVE);
    }
    add(BfAction::SaveAs, "Save as...", LVGL_ICON_SHARED_SAVE_AS);
    add(BfAction::Examples, "Examples", LVGL_ICON_SHARED_LIST_ALT);
    showSelection(ctx, BfDialog::Menu, "Brainfuck", items, icons);
}

static void showExamples(Context* ctx) {
    std::vector<const char*> items;
    std::vector<const char*> icons;
    for (int i = 0; i < NUM_EXAMPLES; i++) {
        items.push_back(examples[i].name);
        icons.push_back(LVGL_ICON_SHARED_LIST_ALT);
    }
    showSelection(ctx, BfDialog::Examples, "Examples", items, icons);
}

/** Runs on the dialog task */
static void onFileDialogResult(const char* const* paths, uint32_t pathCount, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->pickedPath = pathCount > 0 ? paths[0] : "";
    task_event_group_signal(ctx->eventGroup, ctx->fileBit);
}

static void selectFile(Context* ctx, bool saving) {
    char directory[256];
    const bool hasDirectory = getScriptDir(directory, sizeof(directory));
    static const char* const extensions[] = { "bf", "b" };
    const WindowFileDialogOptions options = {
        .flags = saving ? WINDOW_FILE_DIALOG_NEW_FILE : 0U,
        .path = hasDirectory ? directory : nullptr,
        .name = nullptr,
        .extensions = extensions,
        .extension_count = 2,
    };
    ctx->fileSaving = saving;
    if (window_manager_file_dialog(ctx->appInstanceId, &options, onFileDialogResult, ctx) != ERROR_NONE) {
        showStatus(ctx, "Can't open the file selection", LVGL_STATUSLINE_LEVEL_ERROR);
    }
}

void brainfuckDialogResult(Context* ctx) {
    const BfDialog dialog = ctx->dialog.exchange(BfDialog::None);
    const int32_t index = ctx->dialogResult;
    if (index < 0) return;

    if (dialog == BfDialog::Examples && index < NUM_EXAMPLES) {
        setDocument(ctx, examples[index].code, "");
    } else if (dialog == BfDialog::Menu && index < (int32_t)ctx->menuActions.size()) {
        switch (ctx->menuActions[index]) {
            case BfAction::New: setDocument(ctx, "", ""); break;
            case BfAction::Open: selectFile(ctx, false); break;
            case BfAction::Save: save(ctx, ctx->filePath); break;
            case BfAction::SaveAs: selectFile(ctx, true); break;
            case BfAction::Examples: showExamples(ctx); break;
        }
    }
}

void brainfuckFileResult(Context* ctx) {
    std::string path = ctx->pickedPath;
    if (path.empty()) return;

    if (ctx->fileSaving) {
        // A new name without an extension gets one, so "Open" lists the file
        if (path.find('.', path.find_last_of('/') + 1) == std::string::npos) {
            path += ".bf";
        }
        save(ctx, path);
    } else {
        std::string code;
        if (readScript(path, code)) {
            setDocument(ctx, code, path);
        } else {
            showStatus(ctx, "Can't read the file (over 32 KB?)", LVGL_STATUSLINE_LEVEL_ERROR);
        }
    }
}

/* ── Widget creation ──────────────────────────────────────────────── */

static lv_obj_t* createTextArea(lv_obj_t* parent, const char* placeholder) {
    auto* textarea = lv_textarea_create(parent);
    lv_obj_set_width(textarea, LV_PCT(100));
    lv_obj_set_flex_grow(textarea, 1);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_obj_set_style_text_font(textarea, lv_font_get_default(), 0);
    return textarea;
}

void brainfuckCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, 0);

    ctx->toolbar = lvgl_toolbar_create(parent, "");
    updateTitle(ctx);
    lvgl_toolbar_add_text_button_action(ctx->toolbar, LV_SYMBOL_PLAY, onRunClicked, ctx);
    lv_obj_t* menu = lvgl_toolbar_add_text_button_action(ctx->toolbar, LVGL_ICON_SHARED_MORE_VERT, onMenuClicked, ctx);
    lv_obj_set_style_text_font(menu, lvgl_get_shared_icon_default_font(), 0);

    lv_obj_t* content = lv_obj_create(parent);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_style_pad_gap(content, 4, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_scrollable(content, false);

    ctx->inputTa = createTextArea(content, "Type code here, or pick an example from the menu");
    lv_textarea_set_text(ctx->inputTa, ctx->code.c_str());

    ctx->outputTa = createTextArea(content, "Output");
    lv_textarea_set_text(ctx->outputTa, ctx->vm.output);
    lv_textarea_set_cursor_click_pos(ctx->outputTa, false);
    lv_obj_add_state(ctx->outputTa, LV_STATE_DISABLED);
    // Stay disabled (read-only), without the theme fading the text
    lv_obj_set_style_opa(ctx->outputTa, LV_OPA_COVER, LV_STATE_DISABLED);

    ctx->statusline = lvgl_statusline_create(parent);
    lv_obj_set_hidden(ctx->statusline, true);
}

void brainfuckDestroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    // Kept for when the window is rebuilt
    ctx->code = lv_textarea_get_text(ctx->inputTa);
    ctx->inputTa = nullptr;
    ctx->outputTa = nullptr;
    ctx->statusline = nullptr;
    ctx->toolbar = nullptr;
}
