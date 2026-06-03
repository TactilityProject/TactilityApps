#pragma once

#include "View.h"
#include "esp_log.h"

#include <string>
#include <sstream>
#include <lvgl.h>

#include <tt_lvgl.h>

#include <Tactility/RecursiveMutex.h>
#include <Tactility/Thread.h>
#include <TactilityCpp/LvglLock.h>
#include <tactility/device.h>
#include <tactility/drivers/uart_controller.h>

constexpr size_t receiveBufferSize = 512;
constexpr size_t renderBufferSize = receiveBufferSize + 2; // Leave space for newline at split and null terminator at the end

class ConsoleView final : public View {

    const char* TAG = "SerialConsole";

    lv_obj_t* _Nullable parent = nullptr;
    lv_obj_t* _Nullable logTextarea = nullptr;
    lv_obj_t* _Nullable inputTextarea = nullptr;
    Device* uartDev = nullptr;
    std::unique_ptr<tt::Thread> uartThread _Nullable = nullptr;
    bool uartThreadInterrupted = false;
    std::unique_ptr<tt::Thread> viewThread _Nullable = nullptr;
    bool viewThreadInterrupted = false;
    tt::RecursiveMutex mutex;
    uint8_t receiveBuffer[receiveBufferSize];
    uint8_t renderBuffer[renderBufferSize];
    size_t receiveBufferPosition = 0;
    std::string terminatorString = "\n";

    LvglLock lvglLock;

    bool isUartThreadInterrupted() const {
        auto lock = mutex.asScopedLock();
        lock.lock();
        return uartThreadInterrupted;
    }

    bool isViewThreadInterrupted() const {
        auto lock = mutex.asScopedLock();
        lock.lock();
        return viewThreadInterrupted;
    }

    void updateViews() {
        if (parent == nullptr) {
            return;
        }

        // Updating the view is expensive, so we only want to set the text once:
        // Gather all the lines in a single buffer
        if (mutex.lock()) {
            size_t first_part_size = receiveBufferSize - receiveBufferPosition;
            memcpy(renderBuffer, receiveBuffer + receiveBufferPosition, first_part_size);
            renderBuffer[receiveBufferPosition] = '\n';
            if (receiveBufferPosition > 0) {
                memcpy(renderBuffer + first_part_size + 1, receiveBuffer, (receiveBufferSize - first_part_size));
                renderBuffer[receiveBufferSize - 1] = 0x00;
            }
            mutex.unlock();
        }

        if (lvglLock.lock()) {
            lv_textarea_set_text(logTextarea, (const char*)renderBuffer);
            lvglLock.unlock();
        }
    }

    int32_t viewThreadMain() {
        while (!isViewThreadInterrupted()) {
            auto start_time = tt::kernel::getTicks();

            updateViews();

            auto end_time = tt::kernel::getTicks();
            auto time_diff = end_time - start_time;
            auto target_delay = tt::kernel::millisToTicks(500U);
            if (time_diff < target_delay) {
                tt::kernel::delayTicks(target_delay - time_diff);
            }
        }

        return 0;
    }

    int32_t uartThreadMain() {
        while (!isUartThreadInterrupted()) {
            uint8_t byte;
            error_t err = uart_controller_read_byte(uartDev, &byte, tt::kernel::millisToTicks(50));

            // Thread might've been interrupted in the meanwhile
            if (isUartThreadInterrupted()) {
                break;
            }

            if (err == ERROR_NONE) {
                mutex.lock();
                receiveBuffer[receiveBufferPosition++] = byte;
                if (receiveBufferPosition == receiveBufferSize) {
                    receiveBufferPosition = 0;
                }
                mutex.unlock();
            }
        }

        return 0;
    }

    static void onSendClickedCallback(lv_event_t* event) {
        auto* view = (ConsoleView*)lv_event_get_user_data(event);
        view->onSendClicked();
    }

    static void onTerminatorDropdownValueChangedCallback(lv_event_t* event) {
        auto* view = (ConsoleView*)lv_event_get_user_data(event);
        view->onTerminatorDropDownValueChanged(event);
    }

    void onTerminatorDropDownValueChanged(lv_event_t* event) {
        auto* dropdown = static_cast<lv_obj_t*>(lv_event_get_target(event));
        mutex.lock();
        switch (lv_dropdown_get_selected(dropdown)) {
            case 0:
                terminatorString = "\n";
                break;
            case 1:
                terminatorString = "\r\n";
                break;
        }
        mutex.unlock();
    }

    void onSendClicked() {
        mutex.lock();
        std::string input_text = lv_textarea_get_text(inputTextarea);
        std::string to_send;
        to_send.append(input_text + terminatorString);
        Device* localUart = uartDev;
        mutex.unlock();

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

        lv_textarea_set_text(inputTextarea, "");
    }

public:

    void startLogic(Device* dev) {
        assert(dev != nullptr);
        if (dev == nullptr) return;

        memset(receiveBuffer, 0, receiveBufferSize);

        assert(uartThread == nullptr);
        assert(uartDev == nullptr);

        uartDev = dev;

        uartThreadInterrupted = false;
        uartThread = std::make_unique<tt::Thread>(
            "SerConsUart",
            4096,
            [this] { return uartThreadMain(); }
        );
        uartThread->setPriority(tt::Thread::Priority::High);
        uartThread->start();
    }

    void startViews(lv_obj_t* parent) {
        this->parent = parent;

        lv_obj_set_style_pad_gap(parent, 2, 0);

        logTextarea = lv_textarea_create(parent);
        lv_textarea_set_placeholder_text(logTextarea, "Waiting for data...");
        lv_obj_set_flex_grow(logTextarea, 1);
        lv_obj_set_width(logTextarea, LV_PCT(100));
        lv_obj_add_state(logTextarea, LV_STATE_DISABLED);
        lv_obj_set_style_margin_ver(logTextarea, 0, 0);

        auto* input_wrapper = lv_obj_create(parent);
        lv_obj_set_size(input_wrapper, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(input_wrapper, 0, 0);
        lv_obj_set_style_border_width(input_wrapper, 0, 0);
        lv_obj_set_width(input_wrapper, LV_PCT(100));
        lv_obj_set_flex_flow(input_wrapper, LV_FLEX_FLOW_ROW);

        inputTextarea = lv_textarea_create(input_wrapper);
        lv_textarea_set_one_line(inputTextarea, true);
        lv_textarea_set_placeholder_text(inputTextarea, "Text to send");
        lv_obj_set_width(inputTextarea, LV_PCT(100));
        lv_obj_set_flex_grow(inputTextarea, 1);

        auto* terminator_dropdown = lv_dropdown_create(input_wrapper);
        lv_dropdown_set_options(terminator_dropdown, "\\n\n\\r\\n");
        lv_obj_set_width(terminator_dropdown, 70);
        lv_obj_add_event_cb(terminator_dropdown, onTerminatorDropdownValueChangedCallback, LV_EVENT_VALUE_CHANGED, this);

        auto* button = lv_button_create(input_wrapper);
        auto* button_label = lv_label_create(button);
        lv_label_set_text(button_label, "Send");
        lv_obj_add_event_cb(button, onSendClickedCallback, LV_EVENT_SHORT_CLICKED, this);

        viewThreadInterrupted = false;
        viewThread = std::make_unique<tt::Thread>(
            "SerConsView",
            4096,
            [this] { return viewThreadMain(); }
        );
        viewThread->setPriority(tt::Thread::Priority::Higher);
        viewThread->start();
    }

    void stopLogic() {
        auto lock = mutex.asScopedLock();
        lock.lock();

        uartThreadInterrupted = true;

        // Detach thread, it will auto-delete when leaving the current scope
        auto old_uart_thread = std::move(uartThread);
        // Unlock so thread can lock
        lock.unlock();

        if (old_uart_thread->getState() != tt::Thread::State::Stopped) {
            // Wait for thread to finish
            old_uart_thread->join();
        }
    }

    void stopViews() {
        auto lock = mutex.asScopedLock();
        lock.lock();

        viewThreadInterrupted = true;

        // Detach thread, it will auto-delete when leaving the current scope
        auto old_view_thread = std::move(viewThread);

        // Unlock so thread can lock
        lock.unlock();

        if (old_view_thread->getState() != tt::Thread::State::Stopped) {
            // Wait for thread to finish
            old_view_thread->join();
        }
    }

    void stopUart() {
        auto lock = mutex.asScopedLock();
        lock.lock();

        if (uartDev != nullptr) {
            uart_controller_close(uartDev);
            uartDev = nullptr;
        }
    }

    void onStart(lv_obj_t* parent, Device* dev) {
        auto lock = mutex.asScopedLock();
        lock.lock();

        startLogic(dev);
        startViews(parent);
    }

    void onStop() override {
        stopViews();
        stopLogic();
        stopUart();
    }
};
