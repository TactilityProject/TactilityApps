// SPDX-License-Identifier: Apache-2.0
#include <ili9341panel.h>

#include "panel_display.h"

#include <app/event.h>
#include <app/install.h>
#include <app/manager.h>
#include <app/scheduler.h>
#include <app/start.h>
#include <app/stream.h>

#include <lvgl/devices/device_context.h>
#include <lvgl/devices/display.h>
#include <lvgl/fonts.h>
#include <lvgl/icons/shared.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/card.h>
#include <lvgl/widgets/chip.h>
#include <lvgl/widgets/toolbar.h>
#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/concurrent/task_event_group.h>
#include <tactility/device.h>
#include <tactility/drivers/display.h>
#include <tactility/error.h>
#include <tactility/module.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <type_traits>
#include <unistd.h>

namespace {

constexpr int32_t REQUEST_NONE = -1;
constexpr int32_t REQUEST_SAVE = 0;
constexpr int32_t REQUEST_APPLY = 1;
constexpr int32_t REQUEST_REVERT = 2;
constexpr int32_t REQUEST_SET_ENABLED = 3;
/** Posted by buildUi() when the module was started elsewhere (e.g. Kernel settings) while the window was hidden. */
constexpr int32_t REQUEST_REFRESH = 4;
/** Opens the input dialog for a numeric row on the app task. */
constexpr int32_t REQUEST_EDIT_NUMBER = 5;

// This app ships in the same package as the module, which is how the enable switch can start and
// stop it (so the state matches the one in Kernel settings).
constexpr const char* PACKAGE_ID = "tactility.mystifyspidemo";
constexpr const char* MODULE_NAME = "ili9341-panel";
constexpr const char* INPUT_DIALOG_APP_ID = "tactility.inputdialog";

constexpr uint32_t CLOCK_PRESETS[] = { 10, 20, 40, 80 }; // MHz
constexpr uint32_t CLOCK_PRESET_COUNT = sizeof(CLOCK_PRESETS) / sizeof(CLOCK_PRESETS[0]);
constexpr uint32_t HOST_PRESETS[] = { 1, 2 }; // SPI2_HOST, SPI3_HOST
constexpr uint32_t HOST_PRESET_COUNT = sizeof(HOST_PRESETS) / sizeof(HOST_PRESETS[0]);

/** The settings whose values are entered through the input dialog. */
enum class NumericField : uint8_t {
    Width,
    Height,
    MaxTransfer,
    PinSck,
    PinMosi,
    PinMiso,
    PinDc,
    PinReset,
    PinCs,
    Count,
};

constexpr size_t NUMERIC_FIELD_COUNT = static_cast<size_t>(NumericField::Count);

struct NumberDialog {
    NumericField field = NumericField::Width;
    AppInstanceId instanceId = 0;
    AppStream stream {};
    char buffer[32] = {};
};

struct PanelApi {
    decltype(&ili9341panel_settings_defaults) defaults = nullptr;
    decltype(&ili9341panel_settings_load) load = nullptr;
    decltype(&ili9341panel_settings_save) save = nullptr;
    decltype(&ili9341panel_apply) apply = nullptr;
    decltype(&ili9341panel_is_active) isActive = nullptr;
    decltype(&ili9341panel_get_device) getDevice = nullptr;
    Module* module = nullptr;
};

struct Context {
    AppInstanceId appInstanceId = 0;
    PanelApi api {};
    bool moduleAvailable = false;

    // Guards slow work (saving/applying) out of the LVGL task.
    TaskEventGroup* eventGroup = nullptr;
    uint32_t requestBit = 0;
    std::atomic<int32_t> request { REQUEST_NONE };
    std::atomic<bool> enableTarget { false };
    /** NumericField chosen by a row's change button, started as a dialog by the app task. */
    std::atomic<int32_t> editField { -1 };

    // Written by LVGL callbacks (LVGL lock held), read by the app task under the same lock.
    Ili9341PanelConfig config {};

    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* actionStatusLabel = nullptr;
    lv_obj_t* stoppedHint = nullptr;
    lv_obj_t* switchEnable = nullptr;
    lv_obj_t* switchMirrorX = nullptr;
    lv_obj_t* switchMirrorY = nullptr;
    lv_obj_t* switchInvert = nullptr;
    lv_obj_t* switchBgr = nullptr;
    lv_obj_t* orientationChips[4] = {};
    lv_obj_t* resolutionChips[2] = {};
    lv_obj_t* clockChips[CLOCK_PRESET_COUNT] = {};
    lv_obj_t* hostChips[HOST_PRESET_COUNT] = {};
    lv_obj_t* valueLabels[NUMERIC_FIELD_COUNT] = {};
    NumberDialog dialog {};
};

// region Module API

bool resolveSymbol(const char* name, uintptr_t* address, Module** owner) {
    return module_resolve_symbol_global_get(name, address, owner);
}

bool acquireApi(PanelApi& api) {
    Module* module = nullptr;

    auto resolve = [&module](const char* name, auto& function) {
        uintptr_t address = 0;
        Module* owner = nullptr;
        if (!resolveSymbol(name, &address, &owner)) {
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

    const bool found = resolve("ili9341panel_settings_defaults", api.defaults) &&
        resolve("ili9341panel_settings_load", api.load) &&
        resolve("ili9341panel_settings_save", api.save) &&
        resolve("ili9341panel_apply", api.apply) &&
        resolve("ili9341panel_is_active", api.isActive) &&
        resolve("ili9341panel_get_device", api.getDevice);

    if (!found) {
        if (module != nullptr) {
            module_put(module);
        }
        api = PanelApi {};
        return false;
    }

    // Symbol lookup also finds stopped modules (their code stays loaded), so check the state too:
    // otherwise a stopped panel would look "available" and the settings would silently be saved
    // for a device that isn't there.
    if (!module_is_started(module)) {
        module_put(module);
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
    // Clearing the function pointers also makes sure a stale pointer can't be called after the
    // module was stopped and unloaded.
    api = PanelApi {};
}

/** Mirrors ili9341panel_settings_defaults(), so the form is usable while the module is stopped. */
void setFallbackDefaults(Ili9341PanelConfig& config) {
    config = Ili9341PanelConfig {
        .enabled = true,
        .spi_host = 1,
        .pin_sck = 5,
        .pin_mosi = 18,
        .pin_miso = 19,
        .pin_dc = 47,
        .pin_reset = 4,
        .pin_cs = 51,
        .width = 320,
        .height = 240,
        .swap_xy = true,
        .mirror_x = false,
        .mirror_y = false,
        .invert_color = false,
        .bgr_order = true,
        .pixel_clock_hz = 40 * 1000 * 1000,
        .max_transfer_bytes = 16 * 1024,
    };
}

// endregion

// region LVGL <-> module glue

/** Removes the panel's lv_display so the device can be stopped/drawn to. Must not hold the LVGL lock. */
void detachPanelDisplay(Context& ctx, bool clear = false) {
    struct Device* device = nullptr;
    if (ctx.api.getDevice(&device) != ERROR_NONE || device == nullptr) {
        return;
    }
    ili9341panelui::detachFromLvgl(device, clear);
    device_put(device);
}

/** Re-adds the panel as an LVGL display, using the same config as the lvgl module's device scan. */
error_t attachPanelDisplay(Context& ctx) {
    struct Device* device = nullptr;
    error_t result = ctx.api.getDevice(&device);
    if (result != ERROR_NONE || device == nullptr) {
        return result == ERROR_NONE ? ERROR_NOT_FOUND : result;
    }
    result = ili9341panelui::attachToLvgl(device);
    device_put(device);
    return result;
}

// endregion

// region UI helpers

lv_obj_t* createCard(lv_obj_t* parent, lv_coord_t gap) {
    auto* card = lvgl_card_create(parent);
    lv_obj_set_size(card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, gap, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(card, false);
    return card;
}

lv_obj_t* createRow(lv_obj_t* parent, lv_coord_t gap) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(row, gap, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(row, false);
    return row;
}

lv_obj_t* createChip(lv_obj_t* parent, const char* text, lv_coord_t minWidth) {
    auto* chip = lvgl_chip_create(parent);
    if (minWidth > 0) {
        lv_obj_set_style_min_width(chip, minWidth, LV_STATE_DEFAULT);
    }
    auto* label = lv_label_create(chip);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return chip;
}

lv_obj_t* createSectionTitle(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);
    return label;
}

lv_obj_t* createSwitchRow(lv_obj_t* parent, const char* text, bool checked, lv_coord_t gap) {
    auto* row = createRow(parent, gap);
    auto* label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_flex_grow(label, 1);
    auto* control = lv_switch_create(row);
    if (checked) {
        lv_obj_add_state(control, LV_STATE_CHECKED);
    }
    return control;
}

void onValueRowPressed(lv_event_t* event);

/** Creates a mesh-settings style row: title and value on the left, a change button on the right. */
lv_obj_t* createValueRow(lv_obj_t* parent, Context& ctx, const char* title, NumericField field, lv_coord_t gap) {
    auto* row = createRow(parent, gap);

    auto* texts = lv_obj_create(row);
    lv_obj_remove_style_all(texts);
    lv_obj_set_height(texts, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(texts, 1);
    lv_obj_set_flex_flow(texts, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(texts, 0, LV_STATE_DEFAULT);
    lv_obj_set_scrollable(texts, false);

    auto* titleLabel = lv_label_create(texts);
    lv_obj_set_width(titleLabel, LV_PCT(100));
    lv_label_set_long_mode(titleLabel, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(titleLabel, title);

    auto* valueLabel = lv_label_create(texts);
    lv_obj_set_width(valueLabel, LV_PCT(100));
    lv_obj_set_style_text_font(valueLabel, lvgl_get_text_font(FONT_SIZE_SMALL), LV_STATE_DEFAULT);
    lv_label_set_long_mode(valueLabel, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    auto* button = lv_button_create(row);
    lv_obj_set_user_data(button, reinterpret_cast<void*>(static_cast<uintptr_t>(field)));
    auto* icon = lv_label_create(button);
    lv_obj_set_style_text_font(icon, lvgl_get_shared_icon_default_font(), LV_STATE_DEFAULT);
    lv_label_set_text(icon, LVGL_ICON_SHARED_LIST_ALT);
    lv_obj_center(icon);
    lv_obj_add_event_cb(button, onValueRowPressed, LV_EVENT_SHORT_CLICKED, &ctx);

    return valueLabel;
}

void setChipSelected(lv_obj_t* chip, bool selected) {
    if (chip != nullptr) {
        lv_obj_set_state(chip, LV_STATE_CHECKED, selected);
    }
}

int32_t getNumericField(const Context& ctx, NumericField field) {
    switch (field) {
        case NumericField::Width:
            return ctx.config.width;
        case NumericField::Height:
            return ctx.config.height;
        case NumericField::MaxTransfer:
            // Stored in bytes, edited in KB; rounded so 65535 still shows as 64 KB.
            return static_cast<int32_t>((ctx.config.max_transfer_bytes + 512) / 1024);
        case NumericField::PinSck:
            return ctx.config.pin_sck;
        case NumericField::PinMosi:
            return ctx.config.pin_mosi;
        case NumericField::PinMiso:
            return ctx.config.pin_miso;
        case NumericField::PinDc:
            return ctx.config.pin_dc;
        case NumericField::PinReset:
            return ctx.config.pin_reset;
        case NumericField::PinCs:
            return ctx.config.pin_cs;
        default:
            return 0;
    }
}

void setNumericField(Context& ctx, NumericField field, int32_t value) {
    switch (field) {
        case NumericField::Width:
            ctx.config.width = static_cast<uint16_t>(value);
            break;
        case NumericField::Height:
            ctx.config.height = static_cast<uint16_t>(value);
            break;
        case NumericField::MaxTransfer: {
            const uint32_t bytes = static_cast<uint32_t>(value) * 1024;
            ctx.config.max_transfer_bytes = bytes > 65535 ? 65535 : bytes;
            break;
        }
        case NumericField::PinSck:
            ctx.config.pin_sck = value;
            break;
        case NumericField::PinMosi:
            ctx.config.pin_mosi = value;
            break;
        case NumericField::PinMiso:
            ctx.config.pin_miso = value;
            break;
        case NumericField::PinDc:
            ctx.config.pin_dc = value;
            break;
        case NumericField::PinReset:
            ctx.config.pin_reset = value;
            break;
        case NumericField::PinCs:
            ctx.config.pin_cs = value;
            break;
        default:
            break;
    }
}

/** Updates one value label from the working config. Call with the LVGL lock held. */
void updateValueLabel(Context& ctx, NumericField field) {
    auto* label = ctx.valueLabels[static_cast<size_t>(field)];
    if (label == nullptr) {
        return;
    }
    const int32_t value = getNumericField(ctx, field);
    if (field == NumericField::MaxTransfer) {
        lv_label_set_text_fmt(label, "%d KB", static_cast<int>(value));
    } else if (value < 0) {
        lv_label_set_text(label, "None");
    } else {
        lv_label_set_text_fmt(label, "%d", static_cast<int>(value));
    }
}

int orientationIndex(const Ili9341PanelConfig& config) {
    if (config.swap_xy && !config.mirror_x && !config.mirror_y) return 0; // landscape
    if (!config.swap_xy && !config.mirror_x && config.mirror_y) return 1; // portrait
    if (config.swap_xy && config.mirror_x && config.mirror_y) return 2; // landscape, 180
    if (!config.swap_xy && config.mirror_x && !config.mirror_y) return 3; // portrait, 180
    return -1;
}

void applyOrientation(Ili9341PanelConfig& config, int index) {
    switch (index) {
        case 0:
            config.swap_xy = true;
            config.mirror_x = false;
            config.mirror_y = false;
            break;
        case 1:
            // This panel needs mirror Y for portrait; without it the image is upside down.
            config.swap_xy = false;
            config.mirror_x = false;
            config.mirror_y = true;
            break;
        case 2:
            config.swap_xy = true;
            config.mirror_x = true;
            config.mirror_y = true;
            break;
        case 3:
            // ...and mirror X for portrait 180.
            config.swap_xy = false;
            config.mirror_x = true;
            config.mirror_y = false;
            break;
        default:
            break;
    }
}

void refreshOrientationChips(Context& ctx) {
    const int selected = orientationIndex(ctx.config);
    for (int i = 0; i < 4; i++) {
        setChipSelected(ctx.orientationChips[i], i == selected);
    }
}

/** Highlights the size preset that matches the working width/height. Call with the LVGL lock held. */
void refreshResolutionChips(Context& ctx) {
    setChipSelected(ctx.resolutionChips[0], ctx.config.width == 320 && ctx.config.height == 240);
    setChipSelected(ctx.resolutionChips[1], ctx.config.width == 240 && ctx.config.height == 320);
}

void refreshClockChips(Context& ctx) {
    for (uint32_t i = 0; i < CLOCK_PRESET_COUNT; i++) {
        setChipSelected(ctx.clockChips[i], ctx.config.pixel_clock_hz == CLOCK_PRESETS[i] * 1000 * 1000);
    }
}

void refreshHostChips(Context& ctx) {
    for (uint32_t i = 0; i < HOST_PRESET_COUNT; i++) {
        setChipSelected(ctx.hostChips[i], ctx.config.spi_host == static_cast<int32_t>(HOST_PRESETS[i]));
    }
}

/** Writes the working config into every widget. Call with the LVGL lock held. */
void writeConfigToWidgets(Context& ctx) {
    if (ctx.switchEnable != nullptr) {
        // The switch mirrors the module's state, so it always matches Kernel settings.
        lv_obj_set_state(ctx.switchEnable, LV_STATE_CHECKED, ctx.moduleAvailable);
    }
    if (ctx.switchMirrorX != nullptr) {
        lv_obj_set_state(ctx.switchMirrorX, LV_STATE_CHECKED, ctx.config.mirror_x);
    }
    if (ctx.switchMirrorY != nullptr) {
        lv_obj_set_state(ctx.switchMirrorY, LV_STATE_CHECKED, ctx.config.mirror_y);
    }
    if (ctx.switchInvert != nullptr) {
        lv_obj_set_state(ctx.switchInvert, LV_STATE_CHECKED, ctx.config.invert_color);
    }
    if (ctx.switchBgr != nullptr) {
        lv_obj_set_state(ctx.switchBgr, LV_STATE_CHECKED, ctx.config.bgr_order);
    }

    refreshOrientationChips(ctx);
    refreshClockChips(ctx);
    refreshHostChips(ctx);

    for (size_t i = 0; i < NUMERIC_FIELD_COUNT; i++) {
        updateValueLabel(ctx, static_cast<NumericField>(i));
    }

    refreshResolutionChips(ctx);
}

void setActionStatus(Context& ctx, const char* text) {
    if (ctx.actionStatusLabel != nullptr) {
        lv_label_set_text(ctx.actionStatusLabel, text);
    }
}

/** Updates the active/inactive status label. Call with the LVGL lock held. */
void refreshStatus(Context& ctx) {
    if (ctx.statusLabel == nullptr) {
        return;
    }
    if (!ctx.moduleAvailable) {
        lv_label_set_text(ctx.statusLabel, "Stopped");
        return;
    }

    if (ctx.stoppedHint != nullptr) {
        lv_obj_add_flag(ctx.stoppedHint, LV_OBJ_FLAG_HIDDEN);
    }

    if (!ctx.api.isActive()) {
        lv_label_set_text(ctx.statusLabel, "Module running, panel inactive");
        return;
    }

    struct Device* device = nullptr;
    if (ctx.api.getDevice(&device) != ERROR_NONE) {
        lv_label_set_text(ctx.statusLabel, "Active");
        return;
    }

    lv_label_set_text_fmt(
        ctx.statusLabel,
        "Active - %ux%u",
        static_cast<unsigned>(display_get_resolution_x(device)),
        static_cast<unsigned>(display_get_resolution_y(device))
    );
    device_put(device);
}

/** The pickers and chips write ctx.config directly; only reject values that no control produces. */
bool validateConfig(Context& ctx) {
    if (ctx.config.width < 1 || ctx.config.width > 1024 || ctx.config.height < 1 || ctx.config.height > 1024) {
        setActionStatus(ctx, "Width/height must be 1-1024");
        return false;
    }
    if (ctx.config.max_transfer_bytes < 64 || ctx.config.max_transfer_bytes > 65535) {
        setActionStatus(ctx, "Max transfer must be 64-65535 bytes");
        return false;
    }
    return true;
}

// endregion

// region UI events

void onClose(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

void onOrientationPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));
    int index = -1;
    for (int i = 0; i < 4; i++) {
        if (ctx->orientationChips[i] == chip) {
            index = i;
            applyOrientation(ctx->config, i);
            break;
        }
    }
    if (index >= 0) {
        // The presets are tied to the panel's two logical sizes, so portrait also swaps them.
        const bool landscape = index == 0 || index == 2;
        ctx->config.width = landscape ? 320 : 240;
        ctx->config.height = landscape ? 240 : 320;
        updateValueLabel(*ctx, NumericField::Width);
        updateValueLabel(*ctx, NumericField::Height);
        refreshResolutionChips(*ctx);
    }
    refreshOrientationChips(*ctx);
    if (ctx->switchMirrorX != nullptr) {
        lv_obj_set_state(ctx->switchMirrorX, LV_STATE_CHECKED, ctx->config.mirror_x);
    }
    if (ctx->switchMirrorY != nullptr) {
        lv_obj_set_state(ctx->switchMirrorY, LV_STATE_CHECKED, ctx->config.mirror_y);
    }
}

void onClockPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));
    for (uint32_t i = 0; i < CLOCK_PRESET_COUNT; i++) {
        if (ctx->clockChips[i] == chip) {
            ctx->config.pixel_clock_hz = CLOCK_PRESETS[i] * 1000 * 1000;
            break;
        }
    }
    refreshClockChips(*ctx);
}

void onHostPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));
    for (uint32_t i = 0; i < HOST_PRESET_COUNT; i++) {
        if (ctx->hostChips[i] == chip) {
            ctx->config.spi_host = static_cast<int32_t>(HOST_PRESETS[i]);
            break;
        }
    }
    refreshHostChips(*ctx);
}

void onResolutionPresetPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* chip = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));
    const bool landscape = chip == ctx->resolutionChips[0];
    // Match the orientation too, so a size preset can't leave the panel with a mismatched swap_xy.
    applyOrientation(ctx->config, landscape ? 0 : 1);
    ctx->config.width = landscape ? 320 : 240;
    ctx->config.height = landscape ? 240 : 320;
    updateValueLabel(*ctx, NumericField::Width);
    updateValueLabel(*ctx, NumericField::Height);
    refreshOrientationChips(*ctx);
    refreshResolutionChips(*ctx);
    if (ctx->switchMirrorX != nullptr) {
        lv_obj_set_state(ctx->switchMirrorX, LV_STATE_CHECKED, ctx->config.mirror_x);
    }
    if (ctx->switchMirrorY != nullptr) {
        lv_obj_set_state(ctx->switchMirrorY, LV_STATE_CHECKED, ctx->config.mirror_y);
    }
}

// region Number dialog

const char* dialogTitle(NumericField field) {
    switch (field) {
        case NumericField::Width:
            return "Width";
        case NumericField::Height:
            return "Height";
        case NumericField::MaxTransfer:
            return "Max transfer";
        case NumericField::PinSck:
            return "SCK";
        case NumericField::PinMosi:
            return "MOSI";
        case NumericField::PinMiso:
            return "MISO";
        case NumericField::PinDc:
            return "DC";
        case NumericField::PinReset:
            return "RESET";
        case NumericField::PinCs:
            return "CS";
        default:
            return "";
    }
}

const char* dialogMessage(NumericField field) {
    switch (field) {
        case NumericField::Width:
        case NumericField::Height:
            return "1-1024 pixels";
        case NumericField::MaxTransfer:
            return "1-64 KB";
        case NumericField::PinReset:
            return "0-54, or empty for None";
        default:
            return "0-54";
    }
}

/** Digits only, surrounding spaces ignored. */
bool parseUnsigned(const char* text, int32_t* out) {
    if (text == nullptr) {
        return false;
    }
    while (*text == ' ') {
        text++;
    }
    int32_t value = 0;
    bool any = false;
    for (; *text != '\0' && *text != ' '; text++) {
        if (*text < '0' || *text > '9') {
            return false;
        }
        value = value * 10 + (*text - '0');
        any = true;
        if (value > 100000) {
            return false;
        }
    }
    while (*text == ' ') {
        text++;
    }
    if (!any || *text != '\0') {
        return false;
    }
    *out = value;
    return true;
}

int32_t clampField(NumericField field, int32_t value) {
    int32_t minValue = 0;
    int32_t maxValue = 54;
    if (field == NumericField::Width || field == NumericField::Height) {
        minValue = 1;
        maxValue = 1024;
    } else if (field == NumericField::MaxTransfer) {
        minValue = 1;
        maxValue = 64;
    }
    if (value < minValue) {
        value = minValue;
    }
    if (value > maxValue) {
        value = maxValue;
    }
    return value;
}

void openNumberDialog(Context& ctx, NumericField field) {
    if (ctx.dialog.instanceId != 0) {
        return;
    }

    char prefilled[16] = {};
    const int32_t current = getNumericField(ctx, field);
    if (current >= 0) {
        std::snprintf(prefilled, sizeof(prefilled), "%d", static_cast<int>(current));
    }

    const char* argv[] = { dialogTitle(field), dialogMessage(field), prefilled };

    AppStartContext start {};
    error_t result = app_start_context_from_id(INPUT_DIALOG_APP_ID, &start);
    if (result == ERROR_NONE) {
        AppStreamBinding binding = {
            .producer_fd = STDOUT_FILENO,
            .stream = &ctx.dialog.stream,
            .buffer = ctx.dialog.buffer,
            .buffer_capacity = sizeof(ctx.dialog.buffer),
            .event_group = ctx.eventGroup,
            .window_size = {},
            .alias_fd = -1,
        };
        app_start_context_set_arguments_ext(&start, 3, argv);
        app_start_context_set_streams(&start, &binding, 1);
        app_start_context_set_parent(&start, ctx.appInstanceId);

        AppInstanceId instanceId = 0;
        result = app_start_with_context(&start, &instanceId);
        if (result == ERROR_NONE) {
            ctx.dialog.field = field;
            ctx.dialog.instanceId = instanceId;
            return;
        }
    }

    lvgl_lock();
    setActionStatus(ctx, error_to_string(result));
    lvgl_unlock();
}

/** Runs on the app task when APP_EVENT_RESULT for the number dialog arrives. */
void handleNumberDialogResult(Context& ctx, AppInstanceId launchId, int32_t resultCode) {
    const NumericField field = ctx.dialog.field;
    ctx.dialog.instanceId = 0;

    if (resultCode == 0) {
        char text[sizeof(ctx.dialog.buffer)] = {};
        const size_t length = app_stream_read(&ctx.dialog.stream, text, sizeof(text) - 1);
        text[length] = '\0';

        int32_t value = 0;
        if (length == 0 && field == NumericField::PinReset) {
            value = -1;
        } else if (!parseUnsigned(text, &value)) {
            lvgl_lock();
            setActionStatus(ctx, "Enter a whole number");
            lvgl_unlock();
            app_stream_unsubscribe(&ctx.dialog.stream);
            app_manager_stop(launchId);
            return;
        } else {
            value = clampField(field, value);
        }

        lvgl_lock();
        setNumericField(ctx, field, value);
        updateValueLabel(ctx, field);
        refreshResolutionChips(ctx);
        lvgl_unlock();
    }

    app_stream_unsubscribe(&ctx.dialog.stream);
    app_manager_stop(launchId);
}

void onValueRowPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* button = static_cast<lv_obj_t*>(lv_event_get_current_target_obj(event));
    const auto field = static_cast<NumericField>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(button)));
    ctx->editField = static_cast<int32_t>(field);
    ctx->request = REQUEST_EDIT_NUMBER;
    task_event_group_signal(ctx->eventGroup, ctx->requestBit);
}

// endregion

void onEnableChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->enableTarget = lv_obj_has_state(ctx->switchEnable, LV_STATE_CHECKED);
    ctx->request = REQUEST_SET_ENABLED;
    task_event_group_signal(ctx->eventGroup, ctx->requestBit);
}

void onSavePressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->request = REQUEST_SAVE;
    task_event_group_signal(ctx->eventGroup, ctx->requestBit);
}

void onApplyPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    setActionStatus(*ctx, "Applying...");
    ctx->request = REQUEST_APPLY;
    task_event_group_signal(ctx->eventGroup, ctx->requestBit);
}

void onRevertPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->request = REQUEST_REVERT;
    task_event_group_signal(ctx->eventGroup, ctx->requestBit);
}

// endregion

// region Window

void buildUi(lv_obj_t* root, Context& ctx) {
    // The module may have been started from Kernel settings while this window was hidden; the
    // window manager rebuilds the widgets when the app comes back, so re-check here.
    if (!ctx.moduleAvailable && acquireApi(ctx.api)) {
        ctx.moduleAvailable = true;
        if (ctx.api.load(&ctx.config) != ERROR_NONE) {
            ctx.api.defaults(&ctx.config);
        }
        ctx.config.enabled = true;
        ctx.request = REQUEST_REFRESH;
        task_event_group_signal(ctx.eventGroup, ctx.requestBit);
    }

    const bool compact = lvgl_get_ui_density() == LVGL_UI_DENSITY_COMPACT;
    const lv_coord_t pad = compact ? 8 : 12;
    const lv_coord_t gap = compact ? 4 : 8;
    const lv_coord_t chipMinWidth = compact ? 48 : 56;

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(root, false);

    auto* toolbar = lvgl_toolbar_create(root, "ILI9341 Panel");
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onClose, &ctx);

    auto* content = lv_obj_create(root);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(content, LV_PCT(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_pad_all(content, pad, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_gap(content, gap, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(content, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, LV_STATE_DEFAULT);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);

    // Status card
    auto* statusCard = createCard(content, gap);
    ctx.statusLabel = lv_label_create(statusCard);
    lv_obj_set_width(ctx.statusLabel, LV_PCT(100));
    lv_obj_set_style_text_font(ctx.statusLabel, lvgl_get_text_font(FONT_SIZE_LARGE), LV_STATE_DEFAULT);

    ctx.actionStatusLabel = lv_label_create(statusCard);
    lv_obj_set_width(ctx.actionStatusLabel, LV_PCT(100));
    lv_obj_set_style_text_color(ctx.actionStatusLabel, lv_palette_darken(LV_PALETTE_GREY, 3), LV_STATE_DEFAULT);
    lv_label_set_text(ctx.actionStatusLabel, ctx.moduleAvailable ? "Ready" : "Panel is stopped");

    // The switch mirrors the module's started state, so it matches Kernel settings.
    ctx.switchEnable = createSwitchRow(statusCard, "Enabled", ctx.moduleAvailable, gap);
    lv_obj_add_event_cb(ctx.switchEnable, onEnableChanged, LV_EVENT_VALUE_CHANGED, &ctx);

    if (!ctx.moduleAvailable) {
        auto* hint = lv_label_create(statusCard);
        lv_obj_set_width(hint, LV_PCT(100));
        lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
        lv_label_set_text(
            hint,
            "The panel module is stopped; these are built-in defaults. Turn the switch on to use the "
            "panel and load its saved settings."
        );
        ctx.stoppedHint = hint;
    }

    // Panel card
    auto* panelCard = createCard(content, gap);
    createSectionTitle(panelCard, "Panel");

    auto* orientationTitle = lv_label_create(panelCard);
    lv_label_set_text(orientationTitle, "Orientation");

    auto* orientationRow = createRow(panelCard, gap);
    constexpr const char* ORIENTATION_LABELS[] = { "Landscape", "Portrait", "Landscape 180", "Portrait 180" };
    for (int i = 0; i < 4; i++) {
        auto* chip = createChip(orientationRow, ORIENTATION_LABELS[i], chipMinWidth);
        lv_obj_add_event_cb(chip, onOrientationPressed, LV_EVENT_SHORT_CLICKED, &ctx);
        ctx.orientationChips[i] = chip;
    }

    ctx.switchMirrorX = createSwitchRow(panelCard, "Mirror X", ctx.config.mirror_x, gap);
    lv_obj_add_event_cb(ctx.switchMirrorX, [](lv_event_t* event) {
        auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
        ctx->config.mirror_x = lv_obj_has_state(ctx->switchMirrorX, LV_STATE_CHECKED);
        refreshOrientationChips(*ctx);
    }, LV_EVENT_VALUE_CHANGED, &ctx);

    ctx.switchMirrorY = createSwitchRow(panelCard, "Mirror Y", ctx.config.mirror_y, gap);
    lv_obj_add_event_cb(ctx.switchMirrorY, [](lv_event_t* event) {
        auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
        ctx->config.mirror_y = lv_obj_has_state(ctx->switchMirrorY, LV_STATE_CHECKED);
        refreshOrientationChips(*ctx);
    }, LV_EVENT_VALUE_CHANGED, &ctx);

    ctx.switchInvert = createSwitchRow(panelCard, "Invert colours", ctx.config.invert_color, gap);
    lv_obj_add_event_cb(ctx.switchInvert, [](lv_event_t* event) {
        auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
        ctx->config.invert_color = lv_obj_has_state(ctx->switchInvert, LV_STATE_CHECKED);
    }, LV_EVENT_VALUE_CHANGED, &ctx);

    ctx.switchBgr = createSwitchRow(panelCard, "BGR order", ctx.config.bgr_order, gap);
    lv_obj_add_event_cb(ctx.switchBgr, [](lv_event_t* event) {
        auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
        ctx->config.bgr_order = lv_obj_has_state(ctx->switchBgr, LV_STATE_CHECKED);
    }, LV_EVENT_VALUE_CHANGED, &ctx);

    // Size and bus
    auto* sizeCard = createCard(content, gap);
    createSectionTitle(sizeCard, "Size and bus");

    auto* resolutionRow = createRow(sizeCard, gap);
    auto* presetLandscape = createChip(resolutionRow, "320x240", chipMinWidth);
    auto* presetPortrait = createChip(resolutionRow, "240x320", chipMinWidth);
    ctx.resolutionChips[0] = presetLandscape;
    ctx.resolutionChips[1] = presetPortrait;
    lv_obj_add_event_cb(presetLandscape, onResolutionPresetPressed, LV_EVENT_SHORT_CLICKED, &ctx);
    lv_obj_add_event_cb(presetPortrait, onResolutionPresetPressed, LV_EVENT_SHORT_CLICKED, &ctx);

    ctx.valueLabels[static_cast<size_t>(NumericField::Width)] = createValueRow(sizeCard, ctx, "Width", NumericField::Width, gap);
    ctx.valueLabels[static_cast<size_t>(NumericField::Height)] = createValueRow(sizeCard, ctx, "Height", NumericField::Height, gap);

    auto* hostTitle = lv_label_create(sizeCard);
    lv_label_set_text(hostTitle, "SPI host");
    auto* hostRow = createRow(sizeCard, gap);
    constexpr const char* HOST_LABELS[] = { "SPI2", "SPI3" };
    for (uint32_t i = 0; i < HOST_PRESET_COUNT; i++) {
        auto* chip = createChip(hostRow, HOST_LABELS[i], chipMinWidth);
        lv_obj_add_event_cb(chip, onHostPressed, LV_EVENT_SHORT_CLICKED, &ctx);
        ctx.hostChips[i] = chip;
    }

    auto* clockTitle = lv_label_create(sizeCard);
    lv_label_set_text(clockTitle, "SPI clock");
    auto* clockRow = createRow(sizeCard, gap);
    constexpr const char* CLOCK_LABELS[] = { "10 MHz", "20 MHz", "40 MHz", "80 MHz" };
    for (uint32_t i = 0; i < CLOCK_PRESET_COUNT; i++) {
        auto* chip = createChip(clockRow, CLOCK_LABELS[i], chipMinWidth);
        lv_obj_add_event_cb(chip, onClockPressed, LV_EVENT_SHORT_CLICKED, &ctx);
        ctx.clockChips[i] = chip;
    }

    ctx.valueLabels[static_cast<size_t>(NumericField::MaxTransfer)] =
        createValueRow(sizeCard, ctx, "Max transfer", NumericField::MaxTransfer, gap);

    // Pins
    auto* pinsCard = createCard(content, gap);
    createSectionTitle(pinsCard, "Pins");

    ctx.valueLabels[static_cast<size_t>(NumericField::PinSck)] = createValueRow(pinsCard, ctx, "SCK", NumericField::PinSck, gap);
    ctx.valueLabels[static_cast<size_t>(NumericField::PinMosi)] = createValueRow(pinsCard, ctx, "MOSI", NumericField::PinMosi, gap);
    ctx.valueLabels[static_cast<size_t>(NumericField::PinMiso)] = createValueRow(pinsCard, ctx, "MISO", NumericField::PinMiso, gap);
    ctx.valueLabels[static_cast<size_t>(NumericField::PinDc)] = createValueRow(pinsCard, ctx, "DC", NumericField::PinDc, gap);
    ctx.valueLabels[static_cast<size_t>(NumericField::PinReset)] =
        createValueRow(pinsCard, ctx, "RESET (None = unused)", NumericField::PinReset, gap);
    ctx.valueLabels[static_cast<size_t>(NumericField::PinCs)] = createValueRow(pinsCard, ctx, "CS", NumericField::PinCs, gap);

    // Actions
    auto* actionsRow = createRow(content, gap);
    auto* saveChip = createChip(actionsRow, "Save", chipMinWidth);
    lv_obj_add_event_cb(saveChip, onSavePressed, LV_EVENT_SHORT_CLICKED, &ctx);
    auto* applyChip = createChip(actionsRow, "Apply", chipMinWidth);
    lv_obj_add_event_cb(applyChip, onApplyPressed, LV_EVENT_SHORT_CLICKED, &ctx);
    auto* revertChip = createChip(actionsRow, "Revert", chipMinWidth);
    lv_obj_add_event_cb(revertChip, onRevertPressed, LV_EVENT_SHORT_CLICKED, &ctx);

    writeConfigToWidgets(ctx);
    refreshStatus(ctx);
}

void createWidgets(lv_obj_t* root, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    buildUi(root, *ctx);
}

void destroyWidgets(void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->statusLabel = nullptr;
    ctx->actionStatusLabel = nullptr;
    ctx->stoppedHint = nullptr;
    ctx->switchEnable = nullptr;
    ctx->switchMirrorX = nullptr;
    ctx->switchMirrorY = nullptr;
    ctx->switchInvert = nullptr;
    ctx->switchBgr = nullptr;
    for (auto*& chip : ctx->orientationChips) {
        chip = nullptr;
    }
    for (auto*& chip : ctx->resolutionChips) {
        chip = nullptr;
    }
    for (auto*& chip : ctx->clockChips) {
        chip = nullptr;
    }
    for (auto*& chip : ctx->hostChips) {
        chip = nullptr;
    }
    for (auto*& label : ctx->valueLabels) {
        label = nullptr;
    }
}

// endregion

// region Request handling (app task, LVGL lock not held)

/** Starts or stops the panel module, so this switch and Kernel settings always agree. */
void handleSetEnabled(Context& ctx, bool enable) {
    if (enable == ctx.moduleAvailable) {
        lvgl_lock();
        if (ctx.switchEnable != nullptr) {
            lv_obj_set_state(ctx.switchEnable, LV_STATE_CHECKED, ctx.moduleAvailable);
        }
        lvgl_unlock();
        return;
    }

    error_t result = ERROR_NONE;

    if (enable) {
        result = app_module_set_started(PACKAGE_ID, MODULE_NAME, true);
        if (result == ERROR_NONE) {
            ctx.moduleAvailable = acquireApi(ctx.api);
            if (!ctx.moduleAvailable) {
                // Without its API the app can't do anything with the module, so don't leave it running.
                result = ERROR_NOT_FOUND;
                app_module_set_started(PACKAGE_ID, MODULE_NAME, false);
            }
        }
        if (result == ERROR_NONE) {
            Ili9341PanelConfig loaded {};
            if (ctx.api.load(&loaded) == ERROR_NONE) {
                if (!loaded.enabled) {
                    // The module was saved as disabled by an older build; the switch owns that state now.
                    loaded.enabled = true;
                    if (ctx.api.save(&loaded) == ERROR_NONE) {
                        ctx.api.apply();
                    }
                }
                ctx.config = loaded;
                ctx.config.enabled = true;
            } else {
                ctx.api.defaults(&ctx.config);
                ctx.config.enabled = true;
            }
            if (ctx.api.isActive()) {
                const error_t attachResult = attachPanelDisplay(ctx);
                if (attachResult != ERROR_NONE) {
                    result = attachResult;
                }
            }
        }
    } else {
        detachPanelDisplay(ctx, true);
        releaseApi(ctx.api);
        ctx.moduleAvailable = false;
        result = app_module_set_started(PACKAGE_ID, MODULE_NAME, false);
        if (result != ERROR_NONE) {
            // The module is still running (in use elsewhere); reflect that again.
            ctx.moduleAvailable = acquireApi(ctx.api);
            if (ctx.moduleAvailable && ctx.api.isActive()) {
                attachPanelDisplay(ctx);
            }
        }
    }

    lvgl_lock();
    ctx.enableTarget = ctx.moduleAvailable;
    if (ctx.switchEnable != nullptr) {
        lv_obj_set_state(ctx.switchEnable, LV_STATE_CHECKED, ctx.moduleAvailable);
    }
    if (ctx.moduleAvailable) {
        writeConfigToWidgets(ctx);
    }
    refreshStatus(ctx);
    if (result == ERROR_NONE) {
        setActionStatus(ctx, enable ? "Started" : "Stopped");
    } else {
        char buffer[64];
        std::snprintf(
            buffer,
            sizeof(buffer),
            "%s: %s",
            enable ? "Start failed" : "Stop failed",
            error_to_string(result)
        );
        setActionStatus(ctx, buffer);
    }
    lvgl_unlock();
}

void handleSave(Context& ctx, const Ili9341PanelConfig& config) {
    error_t result = ctx.api.save(&config);
    lvgl_lock();
    if (result == ERROR_NONE) {
        ctx.config = config;
        writeConfigToWidgets(ctx);
        setActionStatus(ctx, "Saved");
    } else {
        setActionStatus(ctx, error_to_string(result));
    }
    lvgl_unlock();
}

void handleApply(Context& ctx, const Ili9341PanelConfig& config) {
    // Kept so a configuration that fails to start can't leave the panel stopped until the next save.
    Ili9341PanelConfig previous {};
    const bool hasPrevious = ctx.api.load(&previous) == ERROR_NONE;

    detachPanelDisplay(ctx);

    error_t result = ctx.api.save(&config);
    if (result == ERROR_NONE) {
        result = ctx.api.apply();
    }
    if (result != ERROR_NONE && hasPrevious && ctx.api.save(&previous) == ERROR_NONE) {
        ctx.api.apply();
    }
    if (ctx.api.isActive()) {
        const error_t attachResult = attachPanelDisplay(ctx);
        if (result == ERROR_NONE) {
            result = attachResult;
        }
    }

    lvgl_lock();
    if (result == ERROR_NONE) {
        ctx.config = config;
    } else if (ctx.api.load(&ctx.config) != ERROR_NONE) {
        // Keep the attempted values in the form so they can be corrected.
    }
    ctx.config.enabled = true;
    writeConfigToWidgets(ctx);
    if (result == ERROR_NONE) {
        setActionStatus(ctx, "Applied");
    } else {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "Apply failed: %s", error_to_string(result));
        setActionStatus(ctx, buffer);
    }
    refreshStatus(ctx);
    lvgl_unlock();
}

void handleRevert(Context& ctx) {
    lvgl_lock();
    error_t result = ctx.api.load(&ctx.config);
    if (result == ERROR_NONE) {
        ctx.config.enabled = true;
        writeConfigToWidgets(ctx);
        setActionStatus(ctx, "Reverted to saved settings");
    } else {
        setActionStatus(ctx, error_to_string(result));
    }
    lvgl_unlock();
}

void handleRequest(Context& ctx, int32_t request) {
    if (request == REQUEST_SET_ENABLED) {
        handleSetEnabled(ctx, ctx.enableTarget.load());
        return;
    }

    if (request == REQUEST_EDIT_NUMBER) {
        const int32_t field = ctx.editField.exchange(-1);
        if (field >= 0) {
            openNumberDialog(ctx, static_cast<NumericField>(field));
        }
        return;
    }

    if (request == REQUEST_REFRESH) {
        // buildUi() found the module running after it had been started elsewhere.
        if (!ctx.api.isActive() && !ctx.config.enabled) {
            ctx.config.enabled = true;
            if (ctx.api.save(&ctx.config) == ERROR_NONE) {
                ctx.api.apply();
            }
        }
        if (ctx.api.isActive()) {
            attachPanelDisplay(ctx);
        }
        lvgl_lock();
        refreshStatus(ctx);
        lvgl_unlock();
        return;
    }

    if (request == REQUEST_REVERT) {
        if (!ctx.moduleAvailable) {
            lvgl_lock();
            setActionStatus(ctx, "Enable the panel first");
            lvgl_unlock();
            return;
        }
        handleRevert(ctx);
        return;
    }

    lvgl_lock();
    const bool valid = validateConfig(ctx);
    const Ili9341PanelConfig config = ctx.config;
    lvgl_unlock();
    if (!valid) {
        return;
    }

    // The switch is the only thing that turns the panel on; these actions just fail while it's off.
    if (!ctx.moduleAvailable) {
        lvgl_lock();
        setActionStatus(ctx, "Enable the panel first");
        lvgl_unlock();
        return;
    }

    if (request == REQUEST_SAVE) {
        handleSave(ctx, config);
    } else if (request == REQUEST_APPLY) {
        handleApply(ctx, config);
    }
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
        error_t result = ctx.api.load(&ctx.config);
        if (result != ERROR_NONE) {
            ctx.api.defaults(&ctx.config);
        }
    } else {
        setFallbackDefaults(ctx.config);
    }

    struct TaskEventGroup event_group {};
    task_event_group_construct(&event_group);
    ctx.eventGroup = &event_group;
    check(task_event_group_claim_bit(&event_group, &ctx.requestBit) == ERROR_NONE);

    struct AppEventSubscription subscription {};
    check(app_event_subscribe(&subscription, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create_ext(app_instance_id, createWidgets, destroyWidgets, &ctx);

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        const int32_t request = ctx.request.exchange(REQUEST_NONE);
        if (request != REQUEST_NONE) {
            handleRequest(ctx, request);
        }

        struct AppEvent event {};
        while (app_event_poll(&subscription, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
            if (event.type == APP_EVENT_RESULT && event.result.launch_id == ctx.dialog.instanceId) {
                handleNumberDialogResult(ctx, event.result.launch_id, event.result.result);
            }
        }
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&subscription) == ERROR_NONE);
    task_event_group_release_bit(&event_group, ctx.requestBit);
    task_event_group_destruct(&event_group);
    releaseApi(ctx.api);

    return 0;
}
