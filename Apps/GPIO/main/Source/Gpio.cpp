#include "Gpio.h"

#include <lvgl/fonts.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/device.h>
#include <tactility/drivers/gpio.h>
#include <tactility/log.h>

#include <cstdio>

constexpr auto* TAG = "GPIO";

static void updatePinStates(Context* ctx) {
    // Update pin states
    for (uint32_t i = 0; i < ctx->pinCount; ++i) {
        bool high = false;
        if (ctx->gpioController != nullptr) {
            gpio_controller_get_level(ctx->gpioController, (gpio_pin_t)i, &high);
        }
        ctx->pinStates[i] = high;
    }
}

static void updatePinWidgets(Context* ctx) {
    lvgl_lock();
    for (size_t j = 0; j < ctx->pinCount; ++j) {
        int level = ctx->pinStates[j];
        lv_obj_t* label = ctx->pinWidgets[j];
        void* label_user_data = lv_obj_get_user_data(label);
        // The user data stores the state, so we can avoid unnecessary updates
        if (reinterpret_cast<void*>(level) != label_user_data) {
            lv_obj_set_user_data(label, reinterpret_cast<void*>(level));
            if (level == 0) {
                lv_obj_set_style_bg_color(label, lv_color_make(20, 20, 20), LV_STATE_DEFAULT);
            } else {
                lv_obj_set_style_bg_color(label, lv_color_make(0, 200, 0), LV_STATE_DEFAULT);
            }
        }
    }
    lvgl_unlock();
}

static lv_obj_t* createGpioRowWrapper(lv_obj_t* parent) {
    lv_obj_t* wrapper = lv_obj_create(parent);
    lv_obj_set_style_pad_all(wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_size(wrapper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    return wrapper;
}

// region Task

static void gpioOnTimer(Context* ctx) {
    // Widgets only exist while this window is topmost - skip otherwise (buried by another app
    // started non-modally, e.g. via app_manager_start(); window_manager deletes a buried
    // window's widgets, so touching pinWidgets here would use-after-free them). Same fix as
    // Development.cpp's periodic status timer.
    if (window_manager_get_state(ctx->window) != WINDOW_STATE_GRANTED) {
        return;
    }
    ctx->mutex.lock();
    updatePinStates(ctx);
    updatePinWidgets(ctx);
    ctx->mutex.unlock();
}

static void gpioOnTimerCallback(void* context) {
    gpioOnTimer(static_cast<Context*>(context));
}

// endregion Task

void gpioCreateWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    auto* toolbar = lvgl_toolbar_create(parent, "GPIO");
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);

    // Main content wrapper, enables scrolling content without scrolling the toolbar
    auto* expansion_wrapper = lv_obj_create(parent);
    lv_obj_set_width(expansion_wrapper, LV_PCT(100));
    lv_obj_set_flex_grow(expansion_wrapper, 1);
    lv_obj_set_style_border_width(expansion_wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(expansion_wrapper, 0, LV_STATE_DEFAULT);

    auto* centering_wrapper = lv_obj_create(expansion_wrapper);
    lv_obj_set_size(centering_wrapper, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_align(centering_wrapper, LV_ALIGN_CENTER);
    lv_obj_set_style_border_width(centering_wrapper, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(centering_wrapper, 0, LV_STATE_DEFAULT);

    auto* display = lv_obj_get_display(parent);
    auto horizontal_px = lv_display_get_horizontal_resolution(display);
    auto vertical_px = lv_display_get_vertical_resolution(display);
    bool is_landscape_display = horizontal_px > vertical_px;

    auto block_size = lvgl_get_text_font_height(FONT_SIZE_DEFAULT);
    auto lvgl_density = lvgl_get_ui_density();
    const int32_t square_spacing = (lvgl_density == LVGL_UI_DENSITY_COMPACT) ? 0 : 4;
    int32_t x_spacing = block_size + square_spacing;
    uint8_t column = 0;
    const uint8_t column_limit = is_landscape_display ? 10 : 5;

    auto* row_wrapper = createGpioRowWrapper(centering_wrapper);
    lv_obj_align(row_wrapper, LV_ALIGN_TOP_MID, 0, 0);

    // Measure the widest number label ("00".."<pinCount-1>") once, so the status squares are
    // offset far enough to never overlap it, regardless of the display's font/DPI.
    const int32_t label_gap = (lvgl_density == LVGL_UI_DENSITY_COMPACT) ? 2 : (block_size / 2);
    char max_label_text[8];
    snprintf(max_label_text, sizeof(max_label_text), "%02lu", static_cast<unsigned long>(ctx->pinCount > 0 ? ctx->pinCount - 1 : 0));
    auto* measuring_label = lv_label_create(row_wrapper);
    lv_label_set_text(measuring_label, max_label_text);
    lv_obj_update_layout(measuring_label);
    int32_t offset_from_left_label = lv_obj_get_width(measuring_label) + label_gap;
    lv_obj_del(measuring_label);

    ctx->mutex.lock();

    ctx->pinStates.reset(new uint8_t[ctx->pinCount]());
    ctx->pinWidgets.reset(new lv_obj_t*[ctx->pinCount]());

    for (uint32_t i = 0; i < ctx->pinCount; ++i) {
        // Add the GPIO number before the first item on a row
        if (column == 0) {
            auto* prefix = lv_label_create(row_wrapper);
            lv_label_set_text_fmt(prefix, "%02lu", static_cast<unsigned long>(i));
        }

        // Add a new GPIO status indicator
        auto* status_square = lv_obj_create(row_wrapper);
        lv_obj_set_pos(status_square, column * x_spacing + offset_from_left_label, 0);
        lv_obj_set_size(status_square, block_size, block_size);
        lv_obj_set_style_pad_all(status_square, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_margin_all(status_square, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_border_width(status_square, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_radius(status_square, 0, LV_STATE_DEFAULT);
        lv_obj_set_style_bg_color(status_square, lv_color_make(20, 20, 20), LV_STATE_DEFAULT);
        lv_obj_set_style_bg_opa(status_square, LV_OPA_COVER, LV_STATE_DEFAULT);
        ctx->pinWidgets[i] = status_square;
        ctx->pinStates[i] = false;

        column++;

        if (column >= column_limit) {
            // Add the GPIO number after the last item on a row
            auto* postfix = lv_label_create(row_wrapper);
            lv_label_set_text_fmt(postfix, "%02lu", static_cast<unsigned long>(i));
            lv_obj_set_pos(postfix, column * x_spacing + offset_from_left_label + label_gap, 0);

            // Add a new row wrapper underneath the last one
            auto* new_row_wrapper = createGpioRowWrapper(centering_wrapper);
            lv_obj_align_to(new_row_wrapper, row_wrapper, LV_ALIGN_BOTTOM_LEFT, 0, square_spacing);
            row_wrapper = new_row_wrapper;

            column = 0;
        }
    }

    ctx->mutex.unlock();
}

void gpioInit(Context* ctx) {
    if (device_get_first_active_by_type(&GPIO_CONTROLLER_TYPE, &ctx->gpioController) == ERROR_NONE) {
        gpio_controller_get_pin_count(ctx->gpioController, &ctx->pinCount);
    } else {
        LOG_E(TAG, "No active GPIO controller found");
    }

    ctx->timer = timer_alloc(TIMER_TYPE_PERIODIC, pdMS_TO_TICKS(100), gpioOnTimerCallback, ctx);
}

void gpioTeardown(Context* ctx) {
    ctx->mutex.lock();
    if (ctx->timer != nullptr) {
        timer_stop(ctx->timer);
        timer_free(ctx->timer);
        ctx->timer = nullptr;
    }
    if (ctx->gpioController != nullptr) {
        device_put(ctx->gpioController);
        ctx->gpioController = nullptr;
    }
    ctx->pinWidgets.reset();
    ctx->pinStates.reset();
    ctx->mutex.unlock();
}
