#include "ConsoleView.h"
#include "SerialConsole.h"

#include <esp_log.h>
#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>

#include <Tactility/kernel/Kernel.h>
#include <tactility/device.h>
#include <tactility/drivers/uart_controller.h>

#include <cstring>

namespace {

constexpr auto* TAG = "SerialConsole";

bool isUartThreadInterrupted(ConsoleViewState* state) {
    auto lock = state->mutex.asScopedLock();
    lock.lock();
    return state->uartThreadInterrupted;
}

bool isViewThreadInterrupted(ConsoleViewState* state) {
    auto lock = state->mutex.asScopedLock();
    lock.lock();
    return state->viewThreadInterrupted;
}

void updateViews(Context* app) {
    ConsoleViewState* state = &app->consoleView;

    if (state->parent == nullptr) {
        return;
    }

    // logTextarea only exists while this window is topmost - skip touching it otherwise
    // (window_manager deletes a buried window's widgets, and this runs from a raw background
    // thread that has no other way to know that happened; same reasoning as GPIO.cpp's periodic
    // status timer guard).
    if (window_manager_get_state(app->window) != WINDOW_STATE_GRANTED) {
        return;
    }

    // Updating the view is expensive, so we only want to set the text once:
    // Gather all the lines in a single buffer
    if (state->mutex.lock()) {
        size_t first_part_size = receiveBufferSize - state->receiveBufferPosition;
        memcpy(state->renderBuffer, state->receiveBuffer + state->receiveBufferPosition, first_part_size);
        state->renderBuffer[state->receiveBufferPosition] = '\n';
        if (state->receiveBufferPosition > 0) {
            memcpy(state->renderBuffer + first_part_size + 1, state->receiveBuffer, (receiveBufferSize - first_part_size));
            state->renderBuffer[receiveBufferSize - 1] = 0x00;
        }
        state->mutex.unlock();
    }

    if (lvgl_try_lock(tt::kernel::FREERTOS_MAX_TICKS)) {
        lv_textarea_set_text(state->logTextarea, (const char*)state->renderBuffer);
        lvgl_unlock();
    }
}

int32_t viewThreadMain(Context* app) {
    ConsoleViewState* state = &app->consoleView;
    while (!isViewThreadInterrupted(state)) {
        auto start_time = tt::kernel::getTicks();

        updateViews(app);

        auto end_time = tt::kernel::getTicks();
        auto time_diff = end_time - start_time;
        auto target_delay = tt::kernel::millisToTicks(500U);
        if (time_diff < target_delay) {
            tt::kernel::delayTicks(target_delay - time_diff);
        }
    }

    return 0;
}

int32_t uartThreadMain(Context* app) {
    ConsoleViewState* state = &app->consoleView;
    while (!isUartThreadInterrupted(state)) {
        uint8_t byte;
        error_t err = uart_controller_read_byte(state->uartDev, &byte, tt::kernel::millisToTicks(50));

        // Thread might've been interrupted in the meanwhile
        if (isUartThreadInterrupted(state)) {
            break;
        }

        if (err == ERROR_NONE) {
            state->mutex.lock();
            state->receiveBuffer[state->receiveBufferPosition++] = byte;
            if (state->receiveBufferPosition == receiveBufferSize) {
                state->receiveBufferPosition = 0;
            }
            state->mutex.unlock();
        }
    }

    return 0;
}

void onSendClicked(Context* app) {
    ConsoleViewState* state = &app->consoleView;

    state->mutex.lock();
    std::string input_text = lv_textarea_get_text(state->inputTextarea);
    std::string to_send;
    to_send.append(input_text + state->terminatorString);
    Device* localUart = state->uartDev;
    state->mutex.unlock();

    if (localUart != nullptr) {
        error_t err = uart_controller_write_bytes(
            localUart,
            reinterpret_cast<const uint8_t*>(to_send.c_str()),
            to_send.length(),
            tt::kernel::millisToTicks(100)
        );
        if (err != ERROR_NONE) {
            ESP_LOGE(TAG, "Failed to send \"%s\"", input_text.c_str());
        }
    }

    lv_textarea_set_text(state->inputTextarea, "");
}

void onSendClickedCallback(lv_event_t* event) {
    auto* app = static_cast<Context*>(lv_event_get_user_data(event));
    onSendClicked(app);
}

void onTerminatorDropDownValueChanged(Context* app, lv_event_t* event) {
    ConsoleViewState* state = &app->consoleView;
    auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(event));
    state->mutex.lock();
    switch (lv_dropdown_get_selected(dropdown)) {
        case 0:
            state->terminatorString = "\n";
            break;
        case 1:
            state->terminatorString = "\r\n";
            break;
    }
    state->mutex.unlock();
}

void onTerminatorDropdownValueChangedCallback(lv_event_t* event) {
    auto* app = static_cast<Context*>(lv_event_get_user_data(event));
    onTerminatorDropDownValueChanged(app, event);
}

void startLogic(Context* app, Device* dev) {
    ConsoleViewState* state = &app->consoleView;
    assert(dev != nullptr);
    if (dev == nullptr) return;

    memset(state->receiveBuffer, 0, receiveBufferSize);

    assert(state->uartThread == nullptr);
    assert(state->uartDev == nullptr);

    state->uartDev = dev;

    state->uartThreadInterrupted = false;
    state->uartThread = std::make_unique<tt::Thread>(
        "SerConsUart",
        4096,
        [app] { return uartThreadMain(app); }
    );
    state->uartThread->setPriority(tt::Thread::Priority::High);
    state->uartThread->start();
}

// Builds the console widgets into @a parent. Safe to call more than once for the same session
// (e.g. this window was buried under another app and is now resurfacing) - only touches
// widget-tracking state, never the background threads, which keep running unaffected by burial
// (window_manager only ever destroys the widget tree, never app-owned threads).
void buildWidgets(Context* app, lv_obj_t* parent) {
    ConsoleViewState* state = &app->consoleView;
    state->parent = parent;

    lv_obj_set_style_pad_gap(parent, 2, 0);

    state->logTextarea = lv_textarea_create(parent);
    lv_textarea_set_placeholder_text(state->logTextarea, "Waiting for data...");
    lv_obj_set_flex_grow(state->logTextarea, 1);
    lv_obj_set_width(state->logTextarea, LV_PCT(100));
    lv_obj_add_state(state->logTextarea, LV_STATE_DISABLED);
    lv_obj_set_style_margin_ver(state->logTextarea, 0, 0);

    auto* input_wrapper = lv_obj_create(parent);
    lv_obj_set_size(input_wrapper, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(input_wrapper, 0, 0);
    lv_obj_set_style_border_width(input_wrapper, 0, 0);
    lv_obj_set_width(input_wrapper, LV_PCT(100));
    lv_obj_set_flex_flow(input_wrapper, LV_FLEX_FLOW_ROW);

    state->inputTextarea = lv_textarea_create(input_wrapper);
    lv_textarea_set_one_line(state->inputTextarea, true);
    lv_textarea_set_placeholder_text(state->inputTextarea, "Text to send");
    lv_obj_set_width(state->inputTextarea, LV_PCT(100));
    lv_obj_set_flex_grow(state->inputTextarea, 1);

    auto* terminator_dropdown = lv_dropdown_create(input_wrapper);
    lv_dropdown_set_options(terminator_dropdown, "\\n\n\\r\\n");
    lv_obj_set_width(terminator_dropdown, 70);
    lv_obj_add_event_cb(terminator_dropdown, onTerminatorDropdownValueChangedCallback, LV_EVENT_VALUE_CHANGED, app);

    auto* button = lv_button_create(input_wrapper);
    auto* button_label = lv_label_create(button);
    lv_label_set_text(button_label, "Send");
    lv_obj_add_event_cb(button, onSendClickedCallback, LV_EVENT_SHORT_CLICKED, app);
}

// Starts the periodic UI-refresh thread. Only call once per session (see buildWidgets()'s
// comment on why rebuilding widgets alone is enough on resurface).
void startViewThread(Context* app) {
    ConsoleViewState* state = &app->consoleView;
    state->viewThreadInterrupted = false;
    state->viewThread = std::make_unique<tt::Thread>(
        "SerConsView",
        4096,
        [app] { return viewThreadMain(app); }
    );
    state->viewThread->setPriority(tt::Thread::Priority::Higher);
    state->viewThread->start();
}

void stopLogic(Context* app) {
    ConsoleViewState* state = &app->consoleView;
    auto lock = state->mutex.asScopedLock();
    lock.lock();

    state->uartThreadInterrupted = true;

    // Detach thread, it will auto-delete when leaving the current scope
    auto old_uart_thread = std::move(state->uartThread);
    // Unlock so thread can lock
    lock.unlock();

    if (old_uart_thread->getState() != tt::Thread::State::Stopped) {
        // Wait for thread to finish
        old_uart_thread->join();
    }
}

void stopViews(Context* app) {
    ConsoleViewState* state = &app->consoleView;
    auto lock = state->mutex.asScopedLock();
    lock.lock();

    state->viewThreadInterrupted = true;

    // Detach thread, it will auto-delete when leaving the current scope
    auto old_view_thread = std::move(state->viewThread);

    // Unlock so thread can lock
    lock.unlock();

    if (old_view_thread->getState() != tt::Thread::State::Stopped) {
        // Wait for thread to finish
        old_view_thread->join();
    }
}

void stopUart(Context* app) {
    ConsoleViewState* state = &app->consoleView;
    auto lock = state->mutex.asScopedLock();
    lock.lock();

    if (state->uartDev != nullptr) {
        uart_controller_close(state->uartDev);
        state->uartDev = nullptr;
    }
}

} // namespace

void consoleViewCreate(lv_obj_t* parent, Context* app, Device* dev) {
    ConsoleViewState* state = &app->consoleView;
    auto lock = state->mutex.asScopedLock();
    lock.lock();

    startLogic(app, dev);
    buildWidgets(app, parent);
    startViewThread(app);
}

void consoleViewRebuildWidgets(lv_obj_t* parent, Context* app) {
    ConsoleViewState* state = &app->consoleView;
    auto lock = state->mutex.asScopedLock();
    lock.lock();

    buildWidgets(app, parent);
}

void consoleViewStop(Context* app) {
    stopViews(app);
    stopLogic(app);
    stopUart(app);
}
