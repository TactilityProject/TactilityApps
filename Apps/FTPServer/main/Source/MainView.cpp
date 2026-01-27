#include "MainView.h"
#include <cstdio>
#include <cstring>
#include <tt_app.h>
#include <tt_lvgl_keyboard.h>

void MainView::wifiConnectCallback(lv_event_t* e) {
    tt_app_start("WifiManage");
}

void MainView::onStart(lv_obj_t* parentWidget) {
    parent = parentWidget;

    lv_coord_t screenWidth = lv_obj_get_width(parent);
    bool isSmallScreen = (screenWidth < 280);

    // Main content wrapper
    mainWrapper = lv_obj_create(parent);
    lv_obj_set_size(mainWrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_flex_flow(mainWrapper, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(mainWrapper, isSmallScreen ? 4 : 8, 0);
    lv_obj_set_style_pad_row(mainWrapper, isSmallScreen ? 4 : 6, 0);
    lv_obj_set_style_bg_opa(mainWrapper, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(mainWrapper, 0, 0);
    lv_obj_remove_flag(mainWrapper, LV_OBJ_FLAG_SCROLLABLE);

    // Info panel (IP, status)
    infoPanel = lv_obj_create(mainWrapper);
    lv_obj_set_width(infoPanel, LV_PCT(100));
    lv_obj_set_height(infoPanel, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(infoPanel, isSmallScreen ? 4 : 8, 0);
    lv_obj_set_style_bg_color(infoPanel, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_bg_opa(infoPanel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(infoPanel, 6, 0);
    lv_obj_set_style_border_width(infoPanel, 0, 0);
    lv_obj_remove_flag(infoPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(infoPanel, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(infoPanel, 15, 0);
    lv_obj_set_flex_align(infoPanel, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ipLabel = lv_label_create(infoPanel);
    lv_label_set_text(ipLabel, "IP: --");

    statusLabel = lv_label_create(infoPanel);
    lv_label_set_text(statusLabel, "Ready");

    // Log textarea
    logTextarea = lv_textarea_create(mainWrapper);
    lv_textarea_set_placeholder_text(logTextarea, "FTP activity will appear here...");
    lv_obj_set_width(logTextarea, LV_PCT(100));
    lv_obj_set_flex_grow(logTextarea, 1);
    lv_obj_add_state(logTextarea, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(logTextarea, lv_color_hex(0x0a0a0a), 0);
    lv_obj_set_style_border_color(logTextarea, lv_color_hex(0x333333), 0);
    lv_obj_set_style_radius(logTextarea, 6, 0);
    lv_obj_remove_flag(logTextarea, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_textarea_set_cursor_click_pos(logTextarea, false);
    lv_obj_set_scrollbar_mode(logTextarea, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_state(logTextarea, LV_STATE_FOCUSED);
    tt_lvgl_software_keyboard_hide();
}

void MainView::onStop() {
    statusLabel = nullptr;
    ipLabel = nullptr;
    wifiButton = nullptr;
    wifiCard = nullptr;
    logTextarea = nullptr;
    mainWrapper = nullptr;
    infoPanel = nullptr;
    parent = nullptr;
}

void MainView::updateInfoPanel(const char* ip, const char* status, lv_palette_t color) {
    lv_color_t labelColor = (color != LV_PALETTE_NONE) ? lv_palette_main(color) : lv_color_hex(0xffffff);

    if (ip && ipLabel) {
        lv_label_set_text(ipLabel, ip);
        lv_obj_set_style_text_color(ipLabel, labelColor, 0);
    }
    if (status && statusLabel) {
        lv_label_set_text(statusLabel, status);
        lv_obj_set_style_text_color(statusLabel, labelColor, 0);
    }
}

void MainView::logToScreen(const char* message) {
    if (logTextarea == nullptr || !message || message[0] == '\0') return;

    const int MAX_LINES = 50;

    tt_lvgl_lock(tt::kernel::MAX_TICKS);
    const char* current = lv_textarea_get_text(logTextarea);
    
    // Treat empty string same as nullptr (ignores placeholder)
    if (current && current[0] == '\0') {
        current = nullptr;
    }

    // Count existing lines
    int lineCount = 0;
    if (current && current[0] != '\0') {
        for (const char* p = current; *p; p++) {
            if (*p == '\n') lineCount++;
        }
        lineCount++; // Count the last line (no trailing newline)
    }

    // Only trim if we EXCEED the limit (not equal to it)
    const char* start = current;
    if (lineCount > MAX_LINES && current) {
        start = strchr(current, '\n');
        if (start) start++; // Skip past the newline
        else start = current;
    }

    // Build new text
    char buffer[512];
    if (start && start[0] != '\0') {
        snprintf(buffer, sizeof(buffer), "%s\n%s", start, message);
    } else {
        snprintf(buffer, sizeof(buffer), "%s", message);
    }

    lv_textarea_set_text(logTextarea, buffer);
    lv_obj_scroll_to_y(logTextarea, LV_COORD_MAX, LV_ANIM_ON);
    tt_lvgl_unlock();
}

void MainView::clearLog() {
    if (!logTextarea) return;

    tt_lvgl_lock(tt::kernel::MAX_TICKS);
    lv_textarea_set_text(logTextarea, "");
    tt_lvgl_unlock();
}

void MainView::showWifiPrompt() {
    if (!logTextarea) return;
    if (wifiCard) {
        lv_obj_delete(wifiCard);
        wifiCard = nullptr;
    }

    lv_coord_t width = lv_obj_get_width(mainWrapper);
    bool isSmall = (width < 240);

    wifiCard = lv_obj_create(logTextarea);
    lv_obj_set_size(wifiCard, LV_PCT(90), LV_SIZE_CONTENT);
    lv_obj_center(wifiCard);
    lv_obj_set_style_radius(wifiCard, isSmall ? 8 : 12, 0);
    lv_obj_set_style_bg_color(wifiCard, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(wifiCard, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifiCard, 1, 0);
    lv_obj_set_style_border_color(wifiCard, lv_color_hex(0x444444), 0);
    lv_obj_set_style_pad_all(wifiCard, isSmall ? 10 : 16, 0);
    lv_obj_set_flex_flow(wifiCard, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wifiCard, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(wifiCard, isSmall ? 6 : 10, 0);

    lv_obj_t* wifiIcon = lv_label_create(wifiCard);
    lv_label_set_text(wifiIcon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(wifiIcon, lv_color_hex(0xFF9500), 0);

    lv_obj_t* wifiLabel = lv_label_create(wifiCard);
    lv_label_set_text(wifiLabel, "No Wi-Fi Connection");
    lv_obj_set_style_text_align(wifiLabel, LV_TEXT_ALIGN_CENTER, 0);

    wifiButton = lv_btn_create(wifiCard);
    lv_obj_set_size(wifiButton, isSmall ? 120 : 150, isSmall ? 28 : 34);
    lv_obj_set_style_radius(wifiButton, 6, 0);
    lv_obj_set_style_bg_color(wifiButton, lv_palette_main(LV_PALETTE_BLUE), 0);

    lv_obj_t* btnLabel = lv_label_create(wifiButton);
    lv_label_set_text(btnLabel, "Connect");
    lv_obj_center(btnLabel);

    lv_obj_add_event_cb(wifiButton, wifiConnectCallback, LV_EVENT_CLICKED, nullptr);

    updateInfoPanel(nullptr, "No WiFi", LV_PALETTE_RED);
}
