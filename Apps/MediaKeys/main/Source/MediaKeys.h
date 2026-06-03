#pragma once

#include <TactilityCpp/App.h>
#include <lvgl.h>
#include <tactility/drivers/bluetooth.h>
#include <tactility/drivers/bluetooth_hid_device.h>
#include <tt_app.h>
#include <atomic>

class MediaKeys final : public App {
    // UI elements
    lv_obj_t* _mainWrapper = nullptr;
    lv_obj_t* _switchWidget = nullptr;
    lv_obj_t* _buttonMatrix = nullptr;

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
    static void btEventCallback(struct Device* device, void* context, struct BtEvent event);
    static void sendKeyTask(void* param);

    // Instance methods called by static callbacks
    void handleSwitchToggle(bool enabled);
    void handleButtonPress(uint32_t buttonId);
    void startHid(); // called once radio is confirmed ON

public:

    MediaKeys() = default;
    MediaKeys(const MediaKeys&) = delete;
    MediaKeys& operator=(const MediaKeys&) = delete;
    MediaKeys(MediaKeys&&) = delete;
    MediaKeys& operator=(MediaKeys&&) = delete;

    void onShow(AppHandle appHandle, lv_obj_t* parent) override;
    void onHide(AppHandle appHandle) override;
};
