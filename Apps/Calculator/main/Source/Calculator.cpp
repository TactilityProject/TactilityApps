#include "Calculator.h"

#include <lvgl/widgets/toolbar.h>

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
    return 0;
}

static std::deque<std::string> infixToRPN(const std::string& infix) {
    std::stack<char> opStack;
    std::deque<std::string> output;
    std::string token;
    size_t i = 0;

    while (i < infix.length()) {
        char ch = infix[i];

        if (isdigit(ch)) {
            token.clear();
            while (i < infix.length() && (isdigit(infix[i]) || infix[i] == '.')) { token += infix[i++]; }
            output.push_back(token);
            continue;
        }

        if (ch == '(') { opStack.push(ch); } else if (ch == ')') {
            while (!opStack.empty() && opStack.top() != '(') {
                output.push_back(std::string(1, opStack.top()));
                opStack.pop();
            }
            opStack.pop();
        } else if (strchr("+-*/", ch)) {
            while (!opStack.empty() && precedence(opStack.top()) >= precedence(ch)) {
                output.push_back(std::string(1, opStack.top()));
                opStack.pop();
            }
            opStack.push(ch);
        }

        i++;
    }

    while (!opStack.empty()) {
        output.push_back(std::string(1, opStack.top()));
        opStack.pop();
    }

    return output;
}

static double evaluateRPN(std::deque<std::string> rpnQueue) {
    std::stack<double> values;

    while (!rpnQueue.empty()) {
        std::string token = rpnQueue.front();
        rpnQueue.pop_front();

        if (isdigit(token[0])) {
            double d;
            sscanf(token.c_str(), "%lf", &d);
            values.push(d);
        } else if (strchr("+-*/", token[0])) {
            if (values.size() < 2) return 0;

            double b = values.top();
            values.pop();
            double a = values.top();
            values.pop();

            if (token[0] == '+') values.push(a + b);
            else if (token[0] == '-') values.push(a - b);
            else if (token[0] == '*') values.push(a * b);
            else if (token[0] == '/' && b != 0) values.push(a / b);
        }
    }

    return values.empty() ? 0 : values.top();
}

static double computeFormula(Context* ctx) {
    return evaluateRPN(infixToRPN(std::string(ctx->formulaBuffer)));
}

static void resetCalculator(Context* ctx) {
    memset(ctx->formulaBuffer, 0, sizeof(ctx->formulaBuffer));
    lv_label_set_text(ctx->displayLabel, "0");
    lv_label_set_text(ctx->resultLabel, "");
    ctx->newInput = true;
}

static void evaluateExpression(Context* ctx) {
    double result = computeFormula(ctx);

    size_t formulaLen = strlen(ctx->formulaBuffer);
    size_t maxAvailable = sizeof(ctx->formulaBuffer) - formulaLen - 1;

    if (maxAvailable > 10) {
        char resultBuffer[32];
        snprintf(resultBuffer, sizeof(resultBuffer), " = %.8g", result);
        strncat(ctx->formulaBuffer, resultBuffer, maxAvailable);
    } else {
        snprintf(ctx->formulaBuffer, sizeof(ctx->formulaBuffer), "%.8g", result);
    }

    lv_label_set_text(ctx->displayLabel, "0");
    lv_label_set_text(ctx->resultLabel, ctx->formulaBuffer);
    ctx->newInput = true;
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

    if (strlen(ctx->formulaBuffer) + strlen(txt) < sizeof(ctx->formulaBuffer) - 1) {
        if (ctx->newInput) {
            memset(ctx->formulaBuffer, 0, sizeof(ctx->formulaBuffer));
            ctx->newInput = false;
        }
        strcat(ctx->formulaBuffer, txt);
        lv_label_set_text(ctx->displayLabel, ctx->formulaBuffer);
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

void calculatorCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    lv_obj_t* toolbar = lvgl_toolbar_create(parent, "Calculator");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    lv_obj_t* wrapper = lv_obj_create(parent);
    lv_obj_set_flex_flow(wrapper, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wrapper, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_width(wrapper, LV_PCT(100));
    lv_obj_set_height(wrapper, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(wrapper, 0);
    lv_obj_set_style_pad_top(wrapper, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(wrapper, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_left(wrapper, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_right(wrapper, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(wrapper, 40, LV_PART_MAIN);
    lv_obj_set_style_border_width(wrapper, 0, 0);
    lv_obj_remove_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);

    ctx->displayLabel = lv_label_create(wrapper);
    lv_label_set_text(ctx->displayLabel, "0");
    lv_obj_set_width(ctx->displayLabel, LV_SIZE_CONTENT);
    lv_obj_set_align(ctx->displayLabel, LV_ALIGN_LEFT_MID);

    ctx->resultLabel = lv_label_create(wrapper);
    lv_label_set_text(ctx->resultLabel, "");
    lv_obj_set_width(ctx->resultLabel, LV_SIZE_CONTENT);
    lv_obj_set_align(ctx->resultLabel, LV_ALIGN_RIGHT_MID);

    static const char* btn_map[] = {
        "(", ")", "C", "/", "\n",
        "7", "8", "9", "*", "\n",
        "4", "5", "6", "-", "\n",
        "1", "2", "3", "+", "\n",
        "0", "=", "", "", ""
    };

    lv_obj_t* buttonmatrix = lv_buttonmatrix_create(parent);
    lv_buttonmatrix_set_map(buttonmatrix, btn_map);

    lv_obj_set_style_pad_all(buttonmatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_pad_row(buttonmatrix, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(buttonmatrix, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(buttonmatrix, 2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(buttonmatrix, lv_palette_main(LV_PALETTE_BLUE), LV_PART_ITEMS);

    if (lv_display_get_horizontal_resolution(nullptr) <= 240 || lv_display_get_vertical_resolution(nullptr) <= 240) {
        //small screens
        lv_obj_set_size(buttonmatrix, lv_pct(100), lv_pct(60));
    } else {
        //large screens
        lv_obj_set_size(buttonmatrix, lv_pct(100), lv_pct(80));
    }
    lv_obj_align(buttonmatrix, LV_ALIGN_BOTTOM_MID, 0, -5);

    lv_obj_add_event_cb(buttonmatrix, onButtonPressed, LV_EVENT_VALUE_CHANGED, ctx);
}
