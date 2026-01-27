#include "SettingsView.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

lv_obj_t* SettingsView::createSettingsRow(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

void SettingsView::handleCancel() {
    if (onCancel) {
        onCancel();
    }
}

void SettingsView::handleSave() {
    if (!usernameInput || !passwordInput || !portInput) {
        if (onCancel) {
            onCancel();
        }
        return;
    }

    const char* newUser = lv_textarea_get_text(usernameInput);
    const char* newPass = lv_textarea_get_text(passwordInput);
    const char* newPortStr = lv_textarea_get_text(portInput);

    int port = currentPort;
    if (newPortStr && strlen(newPortStr) > 0) {
        int p = atoi(newPortStr);
        if (p > 0 && p <= 65535) {
            port = p;
        }
    }

    if (onSave) {
        onSave(newUser, newPass, port);
    }
}

void SettingsView::onStart(lv_obj_t* parent, const char* username, const char* password, int port) {
    currentUsername = username;
    currentPassword = password;
    currentPort = port;

    lv_coord_t screenWidth = lv_obj_get_width(parent);
    bool isSmall = (screenWidth < 280);

    // Main container
    lv_obj_t* container = lv_obj_create(parent);
    lv_obj_set_size(container, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(container, 0, 0);
    lv_obj_set_style_pad_all(container, isSmall ? 8 : 16, 0);
    lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(container, isSmall ? 12 : 16, 0);

    // Username row
    lv_obj_t* userRow = createSettingsRow(container);
    lv_obj_t* userLabel = lv_label_create(userRow);
    lv_label_set_text(userLabel, "Username:");

    usernameInput = lv_textarea_create(userRow);
    lv_textarea_set_one_line(usernameInput, true);
    lv_textarea_set_text(usernameInput, username);
    lv_textarea_set_placeholder_text(usernameInput, "user");
    lv_obj_set_width(usernameInput, isSmall ? LV_PCT(55) : LV_PCT(60));
    lv_obj_set_style_bg_color(usernameInput, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(usernameInput, lv_color_hex(0x555555), 0);

    // Password row
    lv_obj_t* passRow = createSettingsRow(container);
    lv_obj_t* passLabel = lv_label_create(passRow);
    lv_label_set_text(passLabel, "Password:");

    // Container for password input + show/hide button
    lv_obj_t* passInputContainer = lv_obj_create(passRow);
    lv_obj_set_width(passInputContainer, isSmall ? LV_PCT(55) : LV_PCT(60));
    lv_obj_set_height(passInputContainer, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(passInputContainer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(passInputContainer, 0, 0);
    lv_obj_set_style_pad_all(passInputContainer, 0, 0);
    lv_obj_set_flex_flow(passInputContainer, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(passInputContainer, 4, 0);

    passwordInput = lv_textarea_create(passInputContainer);
    lv_textarea_set_one_line(passwordInput, true);
    lv_textarea_set_text(passwordInput, password);
    lv_textarea_set_placeholder_text(passwordInput, "pass");
    lv_textarea_set_password_mode(passwordInput, true);
    lv_obj_set_flex_grow(passwordInput, 1);
    lv_obj_set_style_bg_color(passwordInput, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(passwordInput, lv_color_hex(0x555555), 0);

    // Show/hide password button
    showPasswordBtn = lv_btn_create(passInputContainer);
    lv_obj_set_size(showPasswordBtn, isSmall ? 28 : 32, isSmall ? 28 : 32);
    lv_obj_set_style_bg_color(showPasswordBtn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(showPasswordBtn, 4, 0);
    lv_obj_set_style_pad_all(showPasswordBtn, 0, 0);
    lv_obj_add_event_cb(showPasswordBtn, onShowPasswordClickedCallback, LV_EVENT_CLICKED, this);

    lv_obj_t* eyeIcon = lv_label_create(showPasswordBtn);
    lv_label_set_text(eyeIcon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_center(eyeIcon);

    // Port row
    lv_obj_t* portRow = createSettingsRow(container);
    lv_obj_t* portLabel = lv_label_create(portRow);
    lv_label_set_text(portLabel, "Port:");

    portInput = lv_textarea_create(portRow);
    lv_textarea_set_one_line(portInput, true);
    char portStr[8];
    snprintf(portStr, sizeof(portStr), "%d", port);
    lv_textarea_set_text(portInput, portStr);
    lv_textarea_set_accepted_chars(portInput, "0123456789");
    lv_textarea_set_max_length(portInput, 5);
    lv_obj_set_width(portInput, isSmall ? LV_PCT(55) : LV_PCT(60));
    lv_obj_set_style_bg_color(portInput, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_color(portInput, lv_color_hex(0x555555), 0);

    // Spacer
    lv_obj_t* spacer = lv_obj_create(container);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);

    // Button row
    lv_obj_t* btnRow = lv_obj_create(container);
    lv_obj_set_size(btnRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(btnRow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btnRow, 0, 0);
    lv_obj_set_style_pad_all(btnRow, 0, 0);
    lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btnRow, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Cancel button
    lv_obj_t* cancelBtn = lv_btn_create(btnRow);
    lv_obj_set_size(cancelBtn, isSmall ? 90 : 110, isSmall ? 36 : 42);
    lv_obj_set_style_bg_color(cancelBtn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_radius(cancelBtn, 6, 0);
    lv_obj_add_event_cb(cancelBtn, onCancelClickedCallback, LV_EVENT_CLICKED, this);

    lv_obj_t* cancelLabel = lv_label_create(cancelBtn);
    lv_label_set_text(cancelLabel, "Cancel");
    lv_obj_center(cancelLabel);

    // Save button
    lv_obj_t* saveBtn = lv_btn_create(btnRow);
    lv_obj_set_size(saveBtn, isSmall ? 90 : 110, isSmall ? 36 : 42);
    lv_obj_set_style_bg_color(saveBtn, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_radius(saveBtn, 6, 0);
    lv_obj_add_event_cb(saveBtn, onSaveClickedCallback, LV_EVENT_CLICKED, this);

    lv_obj_t* saveLabel = lv_label_create(saveBtn);
    lv_label_set_text(saveLabel, "Save");
    lv_obj_center(saveLabel);
}

void SettingsView::togglePasswordVisibility() {
    passwordVisible = !passwordVisible;
    lv_textarea_set_password_mode(passwordInput, !passwordVisible);

    // Update button icon
    lv_obj_t* btnLabel = lv_obj_get_child(showPasswordBtn, 0);
    if (btnLabel) {
        lv_label_set_text(btnLabel, passwordVisible ? LV_SYMBOL_EYE_OPEN : LV_SYMBOL_EYE_CLOSE);
    }
}

void SettingsView::onStop() {
    usernameInput = nullptr;
    passwordInput = nullptr;
    portInput = nullptr;
    showPasswordBtn = nullptr;
    passwordVisible = false;
}
