#pragma once

#include "View.h"
#include <functional>
#include <lvgl.h>

class SettingsView final : public View {

public:

    typedef std::function<void()> OnCancelFunction;
    typedef std::function<void(const char* username, const char* password, int port)> OnSaveFunction;

private:

    OnCancelFunction onCancel;
    OnSaveFunction onSave;

    lv_obj_t* usernameInput = nullptr;
    lv_obj_t* passwordInput = nullptr;
    lv_obj_t* portInput = nullptr;
    lv_obj_t* showPasswordBtn = nullptr;
    bool passwordVisible = false;

    // Current settings values
    const char* currentUsername;
    const char* currentPassword;
    int currentPort;

    static lv_obj_t* createSettingsRow(lv_obj_t* parent);

    static void onCancelClickedCallback(lv_event_t* event) {
        auto* view = static_cast<SettingsView*>(lv_event_get_user_data(event));
        view->handleCancel();
    }

    static void onSaveClickedCallback(lv_event_t* event) {
        auto* view = static_cast<SettingsView*>(lv_event_get_user_data(event));
        view->handleSave();
    }

    static void onShowPasswordClickedCallback(lv_event_t* event) {
        auto* view = static_cast<SettingsView*>(lv_event_get_user_data(event));
        view->togglePasswordVisibility();
    }

    void handleCancel();
    void handleSave();
    void togglePasswordVisibility();

public:

    SettingsView(OnCancelFunction onCancel, OnSaveFunction onSave)
        : onCancel(std::move(onCancel)), onSave(std::move(onSave)), currentUsername(""), currentPassword(""), currentPort(21) {}

    void onStart(lv_obj_t* parent, const char* username, const char* password, int port);
    void onStop() override;
};
