// SPDX-License-Identifier: Apache-2.0
#include <ili9341panel.h>

#include "panel_display.h"

#include <app/event.h>
#include <app/scheduler.h>

#include <lvgl/devices/device_context.h>
#include <lvgl/devices/display.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/error.h>
#include <tactility/module.h>

#include <esp_heap_caps.h>
#include <esp_timer.h>

#include <cstdio>
#include <ctime>
#include <type_traits>

namespace {

constexpr int32_t REFRESH_PERIOD_MS = 1000;

struct PanelApi {
    decltype(&ili9341panel_get_device) getDevice = nullptr;
    decltype(&ili9341panel_is_active) isActive = nullptr;
    Module* module = nullptr;
};

struct Context {
    AppInstanceId appInstanceId = 0;
    PanelApi api {};
    bool moduleAvailable = false;
    bool panelReady = false;

    struct Device* device = nullptr; // held while the app runs
    lv_display_t* display = nullptr;

    lv_obj_t* panelRoot = nullptr;
    lv_obj_t* panelScreen = nullptr;
    lv_obj_t* timeLabel = nullptr;
    lv_obj_t* dateLabel = nullptr;
    lv_obj_t* uptimeLabel = nullptr;
    lv_obj_t* heapLabel = nullptr;
    lv_obj_t* psramLabel = nullptr;
    lv_timer_t* timer = nullptr;

    lv_obj_t* windowStatusLabel = nullptr;
};

bool acquireApi(PanelApi& api) {
    Module* module = nullptr;

    auto resolve = [&module](const char* name, auto& function) {
        uintptr_t address = 0;
        Module* owner = nullptr;
        if (!module_resolve_symbol_global_get(name, &address, &owner)) {
            return false;
        }
        function = reinterpret_cast<std::remove_reference_t<decltype(function)>>(address);
        if (module == nullptr) {
            module = owner;
        } else {
            module_put(owner);
        }
        return true;
    };

    const bool found = resolve("ili9341panel_get_device", api.getDevice) &&
        resolve("ili9341panel_is_active", api.isActive);
    if (!found || !module_is_started(module)) {
        if (module != nullptr) {
            module_put(module);
        }
        api = PanelApi {};
        return false;
    }

    api.module = module;
    return true;
}

void releaseApi(PanelApi& api) {
    if (api.module != nullptr) {
        module_put(api.module);
    }
    api = PanelApi {};
}

/** Small grey text used for the panel's secondary rows. */
lv_obj_t* createPanelLabel(lv_obj_t* parent, const char* text, uint32_t color) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_SMALL), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(label, lv_color_hex(color), LV_STATE_DEFAULT);
    return label;
}

/** Full-width label with centred text, so alignment doesn't depend on flex item sizing. */
void centerPanelLabel(lv_obj_t* label) {
    lv_obj_set_width(label, LV_PCT(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
}

void updateStats(Context& ctx) {
    if (ctx.timeLabel == nullptr) {
        return;
    }

    char text[48];

    const time_t now = time(nullptr);
    struct tm* local = localtime(&now);
    if (local != nullptr) {
        strftime(text, sizeof(text), "%H:%M:%S", local);
        lv_label_set_text(ctx.timeLabel, text);
        strftime(text, sizeof(text), "%a %d %b", local);
        lv_label_set_text(ctx.dateLabel, text);
    } else {
        lv_label_set_text(ctx.timeLabel, "--:--:--");
        lv_label_set_text(ctx.dateLabel, "no clock");
    }

    const uint64_t totalSeconds = static_cast<uint64_t>(esp_timer_get_time()) / 1000000;
    const unsigned days = static_cast<unsigned>(totalSeconds / 86400);
    const unsigned hours = static_cast<unsigned>((totalSeconds / 3600) % 24);
    const unsigned minutes = static_cast<unsigned>((totalSeconds / 60) % 60);
    const unsigned seconds = static_cast<unsigned>(totalSeconds % 60);
    if (days > 0) {
        std::snprintf(text, sizeof(text), "up %ud %02u:%02u", days, hours, minutes);
    } else {
        std::snprintf(text, sizeof(text), "up %02u:%02u:%02u", hours, minutes, seconds);
    }
    lv_label_set_text(ctx.uptimeLabel, text);

    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    std::snprintf(text, sizeof(text), "RAM %uK", static_cast<unsigned>(internal_free / 1024));
    lv_label_set_text(ctx.heapLabel, text);

    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > 0) {
        std::snprintf(
            text,
            sizeof(text),
            "PSRAM %u.%uM",
            static_cast<unsigned>(psram_free / (1024 * 1024)),
            static_cast<unsigned>((psram_free / (1024 * 102)) % 10)
        );
        lv_label_set_text(ctx.psramLabel, text);
    } else {
        lv_label_set_text(ctx.psramLabel, "PSRAM -");
    }
}

void onTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    updateStats(*ctx);
}

/** Builds the dashboard on the panel's own lv_display. Call with the LVGL lock held. */
void createPanelUi(Context& ctx) {
    if (ctx.display == nullptr || ctx.device == nullptr) {
        return;
    }

    lv_obj_t* screen = lv_display_get_screen_active(ctx.display);
    if (screen == nullptr) {
        return;
    }

    // Paint the screen as well as the container: a display that didn't get the primary theme can
    // otherwise show its own (bright) background behind the labels.
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0D1418), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_STATE_DEFAULT);
    ctx.panelScreen = screen;

    auto* root = lv_obj_create(screen);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, lv_color_hex(0x0D1418), LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(root, 14, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(root, 4, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(root, false);
    // A real column layout: header row, a growing centred block, footer. Nothing is absolutely
    // positioned, so both 320x240 and 240x320 lay out without overlap.
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Header
    auto* headerRow = lv_obj_create(root);
    lv_obj_remove_style_all(headerRow);
    lv_obj_set_size(headerRow, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(headerRow, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(headerRow, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scrollable(headerRow, false);
    createPanelLabel(headerRow, "TAB5", 0x5FA8C8);
    auto* resolution = createPanelLabel(headerRow, "", 0x5FA8C8);
    lv_label_set_text_fmt(
        resolution,
        "%ux%u",
        static_cast<unsigned>(display_get_resolution_x(ctx.device)),
        static_cast<unsigned>(display_get_resolution_y(ctx.device))
    );

    // Clock block, centred in the space between the header and the footer
    auto* center = lv_obj_create(root);
    lv_obj_remove_style_all(center);
    lv_obj_set_width(center, LV_PCT(100));
    lv_obj_set_height(center, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(center, 1);
    lv_obj_set_flex_flow(center, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(center, 2, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(center, false);

    ctx.timeLabel = lv_label_create(center);
    centerPanelLabel(ctx.timeLabel);
    lv_obj_set_style_text_font(ctx.timeLabel, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(ctx.timeLabel, lv_color_hex(0xF2F7FA), LV_STATE_DEFAULT);

    ctx.dateLabel = createPanelLabel(center, "", 0x8AA4B2);
    centerPanelLabel(ctx.dateLabel);

    // Footer: three stacked lines, each short enough for the narrow orientation
    auto* footer = lv_obj_create(root);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(footer, 2, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(footer, false);

    ctx.uptimeLabel = createPanelLabel(footer, "up", 0x8AA4B2);
    ctx.heapLabel = createPanelLabel(footer, "RAM", 0x8AA4B2);
    ctx.psramLabel = createPanelLabel(footer, "PSRAM", 0x8AA4B2);
    centerPanelLabel(ctx.uptimeLabel);
    centerPanelLabel(ctx.heapLabel);
    centerPanelLabel(ctx.psramLabel);

    ctx.panelRoot = root;
    updateStats(ctx);
}

void destroyPanelUi(Context& ctx) {
    lvgl_lock();
    if (ctx.timer != nullptr) {
        lv_timer_delete(ctx.timer);
        ctx.timer = nullptr;
    }
    if (ctx.panelRoot != nullptr) {
        lv_obj_delete(ctx.panelRoot);
        ctx.panelRoot = nullptr;
        ctx.timeLabel = nullptr;
        ctx.dateLabel = nullptr;
        ctx.uptimeLabel = nullptr;
        ctx.heapLabel = nullptr;
        ctx.psramLabel = nullptr;
    }
    if (ctx.panelScreen != nullptr) {
        // Leave the panel dark rather than a stale dashboard-coloured screen.
        lv_obj_set_style_bg_color(ctx.panelScreen, lv_color_hex(0x000000), LV_STATE_DEFAULT);
        ctx.panelScreen = nullptr;
    }
    lvgl_unlock();
}

// region Window

void onClose(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

void createWidgets(lv_obj_t* root, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(root, false);

    auto* toolbar = lvgl_toolbar_create(root, "Panel Dashboard");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onClose, ctx);

    auto* content = lv_obj_create(root);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, 12, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(content, 8, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);

    auto* card = lvgl_card_create(content);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, 8, LV_STATE_DEFAULT);

    auto* title = lv_label_create(card);
    lv_label_set_text(title, "Panel display");
    lv_obj_set_style_text_font(title, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);

    ctx->windowStatusLabel = lv_label_create(card);
    lv_obj_set_width(ctx->windowStatusLabel, LV_PCT(100));
    lv_label_set_long_mode(ctx->windowStatusLabel, LV_LABEL_LONG_WRAP);

    if (ctx->panelReady) {
        lv_label_set_text(
            ctx->windowStatusLabel,
            "The 320x240 panel is showing a live clock, uptime and memory stats. It keeps updating "
            "while this app runs and returns to a blank display when you close it."
        );
    } else if (ctx->moduleAvailable) {
        lv_label_set_text(
            ctx->windowStatusLabel,
            "The panel module is running, but the panel isn't active. Check its wiring and settings "
            "in ILI9341 Panel."
        );
    } else {
        lv_label_set_text(
            ctx->windowStatusLabel,
            "The panel module is stopped. Start it in ILI9341 Panel or Kernel settings, then reopen "
            "this app."
        );
    }
}

void destroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->windowStatusLabel = nullptr;
}

// endregion

} // namespace

extern "C" int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    AppInstanceId app_instance_id = app_scheduler_current_app_id();

    Context ctx;
    ctx.appInstanceId = app_instance_id;
    ctx.moduleAvailable = acquireApi(ctx.api);

    if (ctx.moduleAvailable) {
        if (ctx.api.getDevice(&ctx.device) == ERROR_NONE && ctx.device != nullptr) {
            // A module started at runtime (Kernel settings) has no LVGL display yet; bind it.
            if (ili9341panelui::findLvglDisplay(ctx.device) == nullptr) {
                ili9341panelui::attachToLvgl(ctx.device);
            }
            ctx.display = ili9341panelui::findLvglDisplay(ctx.device);
            ctx.panelReady = ctx.display != nullptr;
        }
    }

    if (ctx.panelReady) {
        lvgl_lock();
        createPanelUi(ctx);
        ctx.timer = lv_timer_create(onTimer, REFRESH_PERIOD_MS, &ctx);
        lvgl_unlock();
    }

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    struct AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create_ext(app_instance_id, createWidgets, destroyWidgets, &ctx);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        struct AppEvent event;
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
    }

    window_manager_remove(window);
    destroyPanelUi(ctx);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    if (ctx.device != nullptr) {
        device_put(ctx.device);
    }
    releaseApi(ctx.api);

    return 0;
}
