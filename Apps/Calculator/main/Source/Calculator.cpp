#include "Calculator.h"

#include <lvgl/widgets/toolbar.h>
#include <tactility/device.h>
#include <tactility/drivers/keyboard.h>

#include <cstdio>
#include <cstring>
#include <ctype.h>
#include <deque>
#include <stack>
#include <string>

constexpr auto* TAG = "Calculator";

static int precedence(char op) {
    if (op == '+' || op == '-') return 1;
    if (op == '*' || op == '/') return 2;
    if (op == '~') return 3; // unary minus marker, binds tighter than * and /
    return 0;
}

static bool infixToRPN(const std::string& infix, std::deque<std::string>& output) {
    std::stack<char> opStack;
    output.clear();
    std::string token;
    size_t i = 0;
    bool expectOperand = true; // true at start, after '(', or after a binary/unary operator

    while (i < infix.length()) {
        char ch = infix[i];

        if (isdigit((unsigned char)ch) || ch == '.') {
            token.clear();
            bool hasDecimalPoint = false;
            while (i < infix.length()) {
                char current = infix[i];
                if (isdigit((unsigned char)current)) {
                    token += current;
                } else if (current == '.' && !hasDecimalPoint) {
                    hasDecimalPoint = true;
                    token += current;
                } else {
                    break;
                }
                ++i;
            }
            if (i + 2 < infix.length() && infix[i] == 'e' && (infix[i + 1] == '+' || infix[i + 1] == '-') && isdigit((unsigned char)infix[i + 2])) {
                token += infix.substr(i, 2);
                i += 2;
                while (i < infix.length() && isdigit((unsigned char)infix[i])) token += infix[i++];
            }
            if (i < infix.length() && infix[i] == '.') return false;
            if (token.find_first_of("0123456789") == std::string::npos) return false; // no digits, e.g. "."
            output.push_back(token);
            expectOperand = false;
            continue;
        }

        if (ch == '(') {
            opStack.push(ch);
            expectOperand = true;
        } else if (ch == ')') {
            while (!opStack.empty() && opStack.top() != '(') {
                output.push_back(std::string(1, opStack.top()));
                opStack.pop();
            }
            if (opStack.empty()) return false; // unmatched ')'
            opStack.pop(); // remove matching '('
            expectOperand = false;
        } else if (ch == '-' && expectOperand) {
            // unary minus: push a marker that binds only to the next operand
            opStack.push('~');
            expectOperand = true;
        } else if (strchr("+-*/", ch)) {
            while (!opStack.empty() && precedence(opStack.top()) >= precedence(ch)) {
                output.push_back(std::string(1, opStack.top()));
                opStack.pop();
            }
            opStack.push(ch);
            expectOperand = true;
        }

        i++;
    }

    while (!opStack.empty()) {
        if (opStack.top() == '(') return false; // unmatched '('
        output.push_back(std::string(1, opStack.top()));
        opStack.pop();
    }

    return true;
}

static bool evaluateRPN(std::deque<std::string> rpnQueue, double& result) {
    std::stack<double> values;

    while (!rpnQueue.empty()) {
        std::string token = rpnQueue.front();
        rpnQueue.pop_front();

        if (isdigit((unsigned char)token[0]) || token[0] == '.') {
            double d;
            sscanf(token.c_str(), "%lf", &d);
            values.push(d);
        } else if (token[0] == '~') {
            if (values.empty()) return false;
            double a = values.top();
            values.pop();
            values.push(-a);
        } else if (strchr("+-*/", token[0])) {
            if (values.size() < 2) return false;

            double b = values.top();
            values.pop();
            double a = values.top();
            values.pop();

            if (token[0] == '+') values.push(a + b);
            else if (token[0] == '-') values.push(a - b);
            else if (token[0] == '*') values.push(a * b);
            else if (token[0] == '/') {
                if (b == 0) return false;
                values.push(a / b);
            }
        }
    }

    if (values.size() != 1) return false;
    result = values.top();
    return true;
}

static bool computeFormula(Context* ctx, double& result) {
    std::deque<std::string> rpn;
    if (!infixToRPN(std::string(ctx->formulaBuffer), rpn) || rpn.empty()) return false;
    return evaluateRPN(std::move(rpn), result);
}

/** The index of "=" in the button map */
static constexpr uint32_t EQUALS_BUTTON = 19;

/** Shows the input, and the last calculation with its result above it */
static void updateDisplay(Context* ctx) {
    lv_label_set_text(ctx->displayLabel, ctx->newInput ? "0" : ctx->formulaBuffer);
    lv_label_set_text(ctx->historyLabel, ctx->history.c_str());
    lv_label_set_text(ctx->resultLabel, ctx->historyResult.c_str());
}

/** "1+2*-3" becomes "1 + 2 * -3" */
static std::string spaced(const char* formula) {
    std::string result;
    char previous = '\0';
    for (const char* c = formula; *c != '\0'; c++) {
        const bool binary = strchr("+-*/", *c) != nullptr && (isdigit((unsigned char)previous) || previous == '.' || previous == ')');
        if (binary) result += ' ';
        result += *c;
        if (binary) result += ' ';
        previous = *c;
    }
    return result;
}

/** Clears the input. The last calculation and its result stay. */
static void resetCalculator(Context* ctx) {
    memset(ctx->formulaBuffer, 0, sizeof(ctx->formulaBuffer));
    ctx->newInput = true;
    ctx->showingResult = false;
    updateDisplay(ctx);
}

static void evaluateExpression(Context* ctx) {
    // Nothing was entered, or nothing changed since the last result
    if (ctx->newInput || ctx->showingResult) return;

    double result;
    ctx->history = spaced(ctx->formulaBuffer);
    if (!computeFormula(ctx, result)) {
        // The input stays, so it can be corrected
        ctx->historyResult = "= Error";
        updateDisplay(ctx);
        return;
    }

    snprintf(ctx->lastResult, sizeof(ctx->lastResult), "%.8g", result);
    ctx->hasLastResult = true;
    ctx->historyResult = std::string("= ") + ctx->lastResult;
    // The result is the input of the next calculation
    snprintf(ctx->formulaBuffer, sizeof(ctx->formulaBuffer), "%s", ctx->lastResult);
    ctx->showingResult = true;
    updateDisplay(ctx);
}

static void deleteLast(Context* ctx) {
    if (ctx->newInput) return;
    const size_t length = strlen(ctx->formulaBuffer);
    if (length > 0) ctx->formulaBuffer[length - 1] = '\0';
    ctx->showingResult = false;
    ctx->newInput = ctx->formulaBuffer[0] == '\0';
    updateDisplay(ctx);
}

static void handleInput(Context* ctx, const char* txt) {
    if (strcmp(txt, "C") == 0) {
        resetCalculator(ctx);
        return;
    }

    if (strcmp(txt, "=") == 0) {
        evaluateExpression(ctx);
        return;
    }

    char resToken[sizeof(ctx->lastResult) + 2];
    if (strcmp(txt, "RES") == 0) {
        if (!ctx->hasLastResult) return;
        if (ctx->lastResult[0] == '-') {
            snprintf(resToken, sizeof(resToken), "(%s)", ctx->lastResult);
        } else {
            snprintf(resToken, sizeof(resToken), "%s", ctx->lastResult);
        }
        txt = resToken;
    }

    // An operator continues from the last result, anything else replaces it
    if (ctx->showingResult) {
        ctx->newInput = strchr("+-*/", txt[0]) == nullptr || txt[1] != '\0';
        ctx->showingResult = false;
    }

    if (strlen(ctx->formulaBuffer) + strlen(txt) < sizeof(ctx->formulaBuffer) - 1) {
        if (ctx->newInput) {
            memset(ctx->formulaBuffer, 0, sizeof(ctx->formulaBuffer));
            ctx->newInput = false;
        }
        strcat(ctx->formulaBuffer, txt);
        updateDisplay(ctx);
    }
}

static void onButtonPressed(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    lv_obj_t* buttonmatrix = lv_event_get_current_target_obj(e);
    lv_event_code_t event_code = lv_event_get_code(e);
    uint32_t button_id = lv_buttonmatrix_get_selected_button(buttonmatrix);
    const char* button_text = lv_buttonmatrix_get_button_text(buttonmatrix, button_id);
    if (event_code == LV_EVENT_VALUE_CHANGED) {
        handleInput(ctx, button_text);
    }
}

/** Typed keys. Arrows and Enter are handled by the button matrix, which presses its selected button. */
static void onKey(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    lv_obj_t* buttonmatrix = lv_event_get_current_target_obj(e);
    const uint32_t key = lv_event_get_key(e);

    if (key == LV_KEY_ESC) {
        // Leave key mode, so the toolbar can be reached
        lv_group_t* group = lv_obj_get_group(buttonmatrix);
        if (group) lv_group_set_editing(group, false);
        return;
    }

    if (key == LV_KEY_BACKSPACE) {
        deleteLast(ctx);
    } else if (key == LV_KEY_DEL || key == 'c' || key == 'C') {
        resetCalculator(ctx);
    } else if (key == '=') {
        evaluateExpression(ctx);
    } else if (key == 'r' || key == 'R') {
        handleInput(ctx, "RES");
    } else if (key > ' ' && key < 127) {
        char text[2] = { static_cast<char>(key), '\0' };
        if (text[0] == 'x' || text[0] == 'X') text[0] = '*';
        if (text[0] == ',') text[0] = '.';
        if (strchr("0123456789.()+-*/", text[0]) == nullptr) return;
        handleInput(ctx, text);
    } else {
        return;
    }

    // So that Enter calculates after typing
    lv_buttonmatrix_set_selected_button(buttonmatrix, EQUALS_BUTTON);
}

static lv_obj_t* createOneLineLabel(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(label, lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)));
    return label;
}

void calculatorCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_scrollable(parent, false);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Calculator");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* wrapper = lv_obj_create(parent);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(wrapper, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(wrapper, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(wrapper, 2, LV_PART_MAIN);
    lv_obj_set_style_border_width(wrapper, 0, 0);
    lv_obj_set_scrollable(wrapper, false);

    // The last calculation on the left, its result on the right
    lv_obj_t* history = lv_obj_create(wrapper);
    lv_obj_remove_style_all(history);
    lv_obj_set_size(history, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(history, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(history, 10, LV_PART_MAIN);
    lv_obj_set_scrollable(history, false);

    // Takes the space that the result leaves, and is cut off with dots when that isn't enough
    ctx->historyLabel = createOneLineLabel(history);
    lv_obj_set_width(ctx->historyLabel, 0);
    lv_obj_set_flex_grow(ctx->historyLabel, 1);
    ctx->resultLabel = lv_label_create(history);

    ctx->displayLabel = lv_label_create(wrapper);
    updateDisplay(ctx);

    static const char* btn_map[] = {
        "(", ")", "C", "/", "\n",
        "7", "8", "9", "*", "\n",
        "4", "5", "6", "-", "\n",
        "1", "2", "3", "+", "\n",
        "0", ".", "RES", "=", ""
    };

    lv_obj_t* buttonmatrix = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(buttonmatrix, btn_map);

    lv_obj_set_style_pad_all(buttonmatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_row(buttonmatrix, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(buttonmatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(buttonmatrix, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(buttonmatrix, lv_palette_main(LV_PALETTE_BLUE), LV_PART_ITEMS);

    lv_obj_set_width(buttonmatrix, LV_PCT(100));
    lv_obj_set_flex_grow(buttonmatrix, 1);

    lv_obj_add_event_cb(buttonmatrix, onButtonPressed, LV_EVENT_VALUE_CHANGED, ctx);
    lv_obj_add_event_cb(buttonmatrix, onKey, LV_EVENT_KEY, ctx);

    lv_group_t* group = lv_group_get_default();
    if (group != nullptr && device_has_active_by_type(&KEYBOARD_TYPE)) {
        lv_group_add_obj(group, buttonmatrix);
        lv_group_focus_obj(buttonmatrix);
        lv_group_set_editing(group, true);
        lv_buttonmatrix_set_selected_button(buttonmatrix, EQUALS_BUTTON);
    }
}
