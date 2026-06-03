#pragma once

#include <TactilityCpp/App.h>
#include <lvgl.h>
#include <tactility/drivers/bluetooth.h>
#include <tactility/drivers/bluetooth_hid_device.h>
#include <tt_app.h>
#include <tt_lvgl_keyboard.h>
#include <atomic>

class MediaKeys final : public App {
    // UI elements
    lv_obj_t* _parent = nullptr;
    lv_obj_t* _mainWrapper = nullptr;
    lv_obj_t* _switchWidget = nullptr;
    lv_obj_t* _buttonMatrix = nullptr;
    lv_timer_t* _keyHighlightTimer = nullptr;
    uint32_t _activeKeyBtn = LV_BTNMATRIX_BTN_NONE;
    bool _keyboardActive = false; // true when matrix has focus group + editing mode

    // HAL device handles
    struct Device* _btDevice = nullptr;
    struct Device* _hidDevice = nullptr;

    // State - accessed from both LVGL thread and BT callback thread
    std::atomic<bool> _isEnabled    {false};
    std::atomic<bool> _radioEnabling{false}; // true while waiting for radio to come ON
    std::atomic<bool> _radioWasOff  {false}; // true if MediaKeys turned the radio on (so we turn it off)

    // Static event callbacks
    static void onSwitchToggled(lv_event_t* e);
    static void onButtonPressed(lv_event_t* e);
    static void onKeyEvent(lv_event_t* e);
    static void onKeyHighlightTimer(lv_timer_t* t);
    static void btEventCallback(struct Device* device, void* context, struct BtEvent event);
    static void sendKeyTask(void* param);

    // Instance methods called by static callbacks
    void handleSwitchToggle(bool enabled);
    void handleButtonPress(uint32_t buttonId);
    void startHid(); // called once radio is confirmed ON
    void enterKeyMode();
    void exitKeyMode();

public:

    MediaKeys() = default;
    MediaKeys(const MediaKeys&) = delete;
    MediaKeys& operator=(const MediaKeys&) = delete;
    MediaKeys(MediaKeys&&) = delete;
    MediaKeys& operator=(MediaKeys&&) = delete;

    void onShow(AppHandle appHandle, lv_obj_t* parent) override;
    void onHide(AppHandle appHandle) override;
};
